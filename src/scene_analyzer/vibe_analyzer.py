#!/usr/bin/env python3
"""
Scene-Aware Video Analysis Pipeline (Python implementation)
ViBe background subtraction + morphological cleanup + CTU-level QP mapping.

Improvements v2:
  - Temporal QP smoothing (EMA) to eliminate flicker
  - Motion-adaptive QP (relax QP on fast-moving ROIs)
  - Per-CTU spatial complexity ceiling (local variance)
  - Edge-aware QP protection (Sobel gradient density)
"""

import numpy as np
from collections import deque
import struct
import os

# ============================================================================
# Configuration Constants
# ============================================================================

VIBE_SAMPLES = 20          # Historical samples per pixel
VIBE_RADIUS = 20           # Color distance threshold
VIBE_MIN_MATCH = 2         # Minimum matches for background classification
VIBE_SUBSAMPLE = 5         # Background update probability 1/N

DOWNSAMPLE_FACTOR = 4      # 1/4 of width and height
CTU_SIZE = 64              # HEVC CTU size
MIN_OBJECT_AREA = 16       # Minimum foreground blob area (in ds pixels)
GHOST_LOCK_FRAMES = 120    # Lock foreground after N stationary frames

QP_ROI = 30.0              # Dynamic ROI block QP (aggressive: was 26)
QP_TRANSITION = 36.0       # Transition zone QP (aggressive: was 32)
QP_BACKGROUND = 46.0       # Static background QP (aggressive: was 42)
QP_NIGHT_CLAMP = 42.0      # Max background QP in night mode (was 36)

ROI_THRESHOLD = 0.10       # p > 10% = ROI
TRANSITION_THRESHOLD = 0.02  # p > 2% = transition
NIGHT_LUMA_THRESHOLD = 40  # Average luma < 40 = night mode
DILATION_CTU = 2           # Dilate ROI by 2 CTUs

# --- New v2 parameters ---
TEMPORAL_SMOOTH_ALPHA = 0.35   # Weight for current frame in EMA (0=fully smoothed, 1=no smoothing)
EDGE_THRESHOLD = 40.0          # Sobel gradient magnitude threshold for edge pixels
LOCAL_VAR_HIGH = 600.0         # Per-CTU variance: protect if above this
LOCAL_VAR_LOW = 150.0          # Per-CTU variance: relax QP if below this
MOTION_HIGH_THRESH = 12.0      # Per-CTU mean frame-diff for motion-adaptive relaxation
QP_DELTA_PROTECT = 2.0         # QP reduction for edge-dense / high-variance CTUs
QP_DELTA_RELAX = 2.0           # QP increase for flat / high-motion CTUs


class ViBeModel:
    """ViBe background subtraction model.
    Each pixel maintains VIBE_SAMPLES historical values.
    A pixel is classified as background if at least VIBE_MIN_MATCH
    samples are within VIBE_RADIUS of the current pixel value.
    """

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.samples = None       # (height, width, VIBE_SAMPLES) uint8
        self.fg_mask = None       # (height, width) uint8
        self.frame_count = 0
        self._rng = np.random.RandomState(1234567)

    def init_from_frame(self, first_frame):
        """Initialize ViBe samples from the first frame using random
        spatial neighbors within +/- 2 pixels."""
        h, w = first_frame.shape
        self.samples = np.zeros((h, w, VIBE_SAMPLES), dtype=np.uint8)
        self.fg_mask = np.zeros((h, w), dtype=np.uint8)

        padded = np.pad(first_frame, 2, mode='edge')
        for n in range(VIBE_SAMPLES):
            dy = self._rng.randint(0, 5, size=(h, w))
            dx = self._rng.randint(0, 5, size=(h, w))
            y_idx = np.arange(h)[:, None] + dy
            x_idx = np.arange(w)[None, :] + dx
            self.samples[:, :, n] = padded[y_idx, x_idx]

        self.frame_count = 1

    def segment(self, frame):
        """Classify each pixel and update the background model."""
        if self.frame_count == 0:
            self.init_from_frame(frame)
            return self.fg_mask

        h, w = frame.shape
        cur = frame.astype(np.int16)

        # Classification: count matches for all pixels at once
        matches = np.zeros((h, w), dtype=np.uint8)
        for n in range(VIBE_SAMPLES):
            diff = np.abs(cur - self.samples[:, :, n].astype(np.int16))
            matches += (diff <= VIBE_RADIUS).astype(np.uint8)

        self.fg_mask = np.where(matches >= VIBE_MIN_MATCH, 0, 255).astype(np.uint8)

        # Conservative update: random sample replacement for background pixels
        bg_mask = self.fg_mask == 0
        update_mask = self._rng.randint(0, VIBE_SUBSAMPLE, size=(h, w)) == 0
        update_mask &= bg_mask

        if update_mask.any():
            n_idx = self._rng.randint(0, VIBE_SAMPLES, size=(h, w))
            for n in range(VIBE_SAMPLES):
                layer_update = update_mask & (n_idx == n)
                if layer_update.any():
                    self.samples[:, :, n][layer_update] = frame[layer_update]

        # Spatial diffusion: randomly update neighbor's sample
        diff_mask = self._rng.randint(0, VIBE_SUBSAMPLE, size=(h, w)) == 0
        diff_mask &= bg_mask
        if diff_mask.any():
            dy = self._rng.randint(-3, 4, size=(h, w))
            dx = self._rng.randint(-3, 4, size=(h, w))
            yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing='ij')
            ny = np.clip(yy + dy, 0, h - 1)
            nx = np.clip(xx + dx, 0, w - 1)
            n_idx = self._rng.randint(0, VIBE_SAMPLES, size=(h, w))
            for n in range(VIBE_SAMPLES):
                layer = diff_mask & (n_idx == n)
                if layer.any():
                    self.samples[ny[layer], nx[layer], n] = frame[layer]

        self.frame_count += 1
        return self.fg_mask


class Morphology:
    """Binary morphological operations on uint8 masks (255=foreground, 0=background).
    All functions accept and return uint8 arrays with values 0 or 255."""

    @staticmethod
    def erode_3x3(mask):
        """Erode: keep pixel if ALL 3x3 neighbors are foreground."""
        from scipy.ndimage import minimum_filter
        eroded = (minimum_filter(mask, size=3) == 255)
        return eroded.astype(np.uint8) * 255

    @staticmethod
    def dilate_3x3(mask):
        """Dilate: set pixel if ANY 3x3 neighbor is foreground."""
        from scipy.ndimage import maximum_filter
        dilated = (maximum_filter(mask, size=3) > 0)
        return dilated.astype(np.uint8) * 255

    @staticmethod
    def open_3x3(mask):
        """Opening: erode then dilate (removes noise)."""
        eroded = Morphology.erode_3x3(mask)
        return Morphology.dilate_3x3(eroded)

    @staticmethod
    def dilate_n(mask, iterations):
        """N-iteration dilation for CTU-level safety margin."""
        result = mask.copy()
        for _ in range(iterations):
            result = Morphology.dilate_3x3(result)
        return result


class ConnectedComponents:
    """Two-pass connected component labeling on binary mask."""

    @staticmethod
    def label(mask, min_area=MIN_OBJECT_AREA):
        """Return list of (x, y, w, h, area, center_x, center_y) for each blob."""
        from scipy.ndimage import label, find_objects
        labeled, num_features = label(mask)
        blobs = []
        slices = find_objects(labeled)
        for i, slc in enumerate(slices):
            if slc is None:
                continue
            region = (labeled[slc] == (i + 1))
            area = region.sum()
            if area < min_area:
                continue
            y_slice, x_slice = slc
            y, x = y_slice.start, x_slice.start
            h = y_slice.stop - y_slice.start
            w = x_slice.stop - x_slice.start
            ys, xs = np.where(region)
            cx = int(np.mean(xs)) + x
            cy = int(np.mean(ys)) + y
            blobs.append((x, y, w, h, area, cx, cy))
        return blobs


class ObjectTracker:
    """Tracks foreground objects across frames to detect ghost/traffic-light patterns."""

    def __init__(self, ghost_lock_frames=GHOST_LOCK_FRAMES):
        self.objects = []    # List of dicts: {id, x, y, w, h, cx, cy, cx_prev, cy_prev,
                              #                    static_frames, locked, active}
        self.next_id = 0
        self.ghost_lock_frames = ghost_lock_frames

    def update(self, blobs, mask, max_dist):
        """Match blobs to tracked objects, enforce ghost lock."""
        for obj in self.objects:
            obj['active'] = False

        for blob in blobs:
            x, y, w, h, area, cx, cy = blob
            matched = -1
            best_dist = max_dist ** 2

            for j, obj in enumerate(self.objects):
                if obj['active']:
                    continue
                dx = cx - obj['cx']
                dy = cy - obj['cy']
                dist2 = dx * dx + dy * dy
                if dist2 < best_dist:
                    best_dist = dist2
                    matched = j

            if matched >= 0:
                obj = self.objects[matched]
                obj['active'] = True
                obj['cx_prev'] = obj['cx']
                obj['cy_prev'] = obj['cy']
                obj['x'] = x; obj['y'] = y; obj['w'] = w; obj['h'] = h
                obj['cx'] = cx; obj['cy'] = cy

                move = (cx - obj['cx_prev']) ** 2 + (cy - obj['cy_prev']) ** 2
                if move < 4:
                    obj['static_frames'] += 1
                    if obj['static_frames'] >= self.ghost_lock_frames:
                        obj['locked'] = True
                else:
                    obj['static_frames'] = 0
            else:
                self.objects.append({
                    'id': self.next_id,
                    'x': x, 'y': y, 'w': w, 'h': h,
                    'cx': cx, 'cy': cy,
                    'cx_prev': cx, 'cy_prev': cy,
                    'static_frames': 0,
                    'locked': False,
                    'active': True
                })
                self.next_id += 1

        self.objects = [obj for obj in self.objects if obj['active'] or obj['locked']]

        for obj in self.objects:
            if obj['locked']:
                x0 = max(0, obj['x'])
                y0 = max(0, obj['y'])
                x1 = min(mask.shape[1], obj['x'] + obj['w'])
                y1 = min(mask.shape[0], obj['y'] + obj['h'])
                mask[y0:y1, x0:x1] = 255

        return mask


class SceneAnalyzer:
    """Main scene analysis pipeline — v2 with temporal smoothing,
    motion-adaptive QP, per-CTU spatial complexity, and edge-aware QP."""

    def __init__(self, src_width, src_height, ctu_size=CTU_SIZE, downsample_factor=DOWNSAMPLE_FACTOR):
        self.src_width = src_width
        self.src_height = src_height
        self.ctu_size = ctu_size
        self.ds_factor = downsample_factor

        self.ds_width = src_width // downsample_factor
        self.ds_height = src_height // downsample_factor
        self.map_w = (src_width + ctu_size - 1) // ctu_size
        self.map_h = (src_height + ctu_size - 1) // ctu_size
        self.ctu_count = self.map_w * self.map_h

        self.vibe = ViBeModel(self.ds_width, self.ds_height)
        self.tracker = ObjectTracker()

        self.frame_index = 0
        self.night_mode = False
        self.metadata = {}

        # Foreground ratio history for GOP suggestion
        self.fg_history = deque(maxlen=300)
        self.consecutive_static = 0
        self.motion_onset = False

        # IDR cooldown
        self.idr_cooldown = 0
        self.min_idr_interval = 30

        # GOP size parameters
        self.static_gop_size = 300
        self.dynamic_gop_size = 60
        self.normal_gop_size = 120
        self.current_gop_size = self.normal_gop_size

        self.idr_pending = False

        # Pre-allocated buffers
        self.ds_y = np.zeros((self.ds_height, self.ds_width), dtype=np.uint8)
        self.mask_ds = np.zeros((self.ds_height, self.ds_width), dtype=np.uint8)
        self.qp_map = np.zeros((self.map_h, self.map_w), dtype=np.float32)

        # --- v2: New buffers for temporal smoothing, motion, edges ---
        self.prev_qp_map = None                 # Previous frame's smoothed QP map
        self.prev_ds_y = None                   # Previous frame's ds_y for motion detection
        self.edge_map = np.zeros((self.ds_height, self.ds_width), dtype=np.float32)
        self.motion_map = np.zeros((self.ds_height, self.ds_width), dtype=np.float32)

        # Tune these per use case
        self.temporal_alpha = TEMPORAL_SMOOTH_ALPHA
        self.edge_threshold = EDGE_THRESHOLD
        self.local_var_high = LOCAL_VAR_HIGH
        self.local_var_low = LOCAL_VAR_LOW
        self.motion_high_thresh = MOTION_HIGH_THRESH
        self.qp_delta_protect = QP_DELTA_PROTECT
        self.qp_delta_relax = QP_DELTA_RELAX

    def downsample(self, y_frame):
        """Downsample Y plane by factor using block averaging."""
        h, w = y_frame.shape
        factor = self.ds_factor
        ds_h = h // factor
        ds_w = w // factor
        cropped = y_frame[:ds_h * factor, :ds_w * factor]
        reshaped = cropped.reshape(ds_h, factor, ds_w, factor)
        self.ds_y = reshaped.mean(axis=(1, 3)).astype(np.uint8)
        return self.ds_y

    def _compute_edge_map(self):
        """Compute edge magnitude using Sobel gradients on downsampled Y plane.
        Result stored in self.edge_map as float32 gradient magnitude."""
        from scipy.ndimage import sobel
        fy = self.ds_y.astype(np.float32)
        gx = sobel(fy, axis=1, mode='nearest')
        gy = sobel(fy, axis=0, mode='nearest')
        self.edge_map = np.abs(gx) + np.abs(gy)

    def _compute_motion_map(self):
        """Compute per-pixel frame difference as motion intensity proxy.
        Frame 0 has no previous frame → motion_map is all zeros."""
        if self.prev_ds_y is not None:
            diff = np.abs(self.ds_y.astype(np.float32) - self.prev_ds_y.astype(np.float32))
            self.motion_map = diff
        else:
            self.motion_map.fill(0.0)

    def process_frame(self, y_frame):
        """Process one Y frame through the full pipeline.
        Returns (qp_map_1d, metadata_dict)"""
        # Step 1: Downsample
        self.downsample(y_frame)

        # Step 1b (v2): Compute edge map and motion map BEFORE ViBe
        self._compute_edge_map()
        self._compute_motion_map()

        # Step 2: Global luma detection
        avg_luma = float(self.ds_y.mean())
        self.night_mode = (avg_luma < NIGHT_LUMA_THRESHOLD)

        # Step 3: ViBe background subtraction
        self.mask_ds = self.vibe.segment(self.ds_y)

        # Step 4: Morphological opening (denoising)
        self.mask_ds = Morphology.open_3x3(self.mask_ds)

        # Step 5: Connected components and tracking
        blobs = ConnectedComponents.label(self.mask_ds)
        max_dist = self.ds_width // 8
        self.mask_ds = self.tracker.update(blobs, self.mask_ds, max_dist)

        # Step 6: Generate CTU-level QP map (v2: with per-CTU adjustments)
        self._generate_qp_map()

        # Step 6b (v2): Temporal smoothing of QP map (EMA)
        if self.prev_qp_map is not None and self.temporal_alpha < 1.0:
            self.qp_map = (self.temporal_alpha * self.qp_map +
                           (1.0 - self.temporal_alpha) * self.prev_qp_map)
        self.prev_qp_map = self.qp_map.copy()

        # Step 7: Update foreground history and detect motion onset
        fg_ratio = float((self.mask_ds == 255).mean())
        self.fg_history.append(fg_ratio)

        # Motion-onset detection
        self.motion_onset = False
        if fg_ratio < 0.02:
            self.consecutive_static += 1
        else:
            if self.consecutive_static > 60:
                self.motion_onset = True
            self.consecutive_static = 0

        # Step 8: Dynamic GOP size selection
        self.current_gop_size = self._suggest_gop()

        # Step 9: IDR cooldown tick
        if self.idr_cooldown > 0:
            self.idr_cooldown -= 1

        # Step 10: Check if IDR should be forced
        self.idr_pending = self.should_force_idr()
        if self.idr_pending:
            self.idr_cooldown = self.min_idr_interval

        # Step 11: Metadata
        self.metadata = {
            'frame_index': self.frame_index,
            'foreground_ratio': fg_ratio,
            'avg_luma': avg_luma,
            'is_night_mode': self.night_mode,
            'is_static_scene': self.consecutive_static > 150,
            'num_objects': len([o for o in self.tracker.objects if o['active'] or o['locked']]),
            'suggested_gop': self.current_gop_size,
            'motion_onset': self.motion_onset,
            'idr_pending': self.idr_pending,
            'consecutive_static': self.consecutive_static,
        }

        # Step 12 (v2): Save ds_y for next frame's motion detection
        self.prev_ds_y = self.ds_y.copy()

        self.frame_index += 1
        return self.qp_map.flatten().copy(), self.metadata.copy()

    def _adaptive_qp_params(self):
        """QP selection: fixed defaults + spatial complexity ceiling + night mode.

        Returns (roi_qp, transition_qp, background_qp, night_clamp, roi_thresh)."""

        # Aggressive: target SSIM just above 0.92 (was 26/32/42)
        roi, trans, bg = 30, 36, 46
        roi_thresh = 0.10

        # Global spatial complexity ceiling (lowered to 1000 to protect moderate-texture scenes)
        spatial_var = float(np.var(self.ds_y))
        if spatial_var > 1000:
            bg = 42
            trans = 38
            roi = 32

        # Night mode: hard QP ceiling
        night_clamp = bg
        if self.night_mode:
            if bg > 42:
                bg = 42
            if roi > 28:
                roi = 28
            if trans > 36:
                trans = 36
            night_clamp = 42

        return roi, trans, bg, night_clamp, roi_thresh

    def _generate_qp_map(self):
        """Generate CTU-level QP map with per-CTU adjustments (v2):

        Base QP from mask ratio → per-CTU adjustments:
          - Edge-dense CTU: protect (-qp_delta_protect)
          - High local variance CTU: protect (-qp_delta_protect)
          - High motion CTU: relax (+qp_delta_relax)
          - Flat / low-variance CTU: relax (+qp_delta_relax)

        Adjustments are additive/capped. Night mode hard-ceiling applied last."""
        roi_qp, trans_qp, bg_qp, night_clamp, roi_thresh = self._adaptive_qp_params()

        self.qp_map.fill(bg_qp)

        ctu_ds = self.ctu_size // self.ds_factor
        trans_thresh = roi_thresh * 0.2

        for cy in range(self.map_h):
            y0 = cy * ctu_ds
            y1 = min(y0 + ctu_ds, self.ds_height)
            for cx in range(self.map_w):
                x0 = cx * ctu_ds
                x1 = min(x0 + ctu_ds, self.ds_width)

                region = self.mask_ds[y0:y1, x0:x1]
                total = region.size
                if total == 0:
                    continue
                fg_count = int((region == 255).sum())
                p = fg_count / total

                # Base QP by category
                if p >= roi_thresh:
                    base_qp = roi_qp
                elif p >= trans_thresh:
                    base_qp = trans_qp
                else:
                    base_qp = bg_qp

                # --- Per-CTU adjustments (v2) ---

                # Edge density: protect edge-rich CTUs
                edge_region = self.edge_map[y0:y1, x0:x1]
                edge_density = float((edge_region > self.edge_threshold).mean())
                if edge_density > 0.15:
                    base_qp -= self.qp_delta_protect

                # Local spatial variance: protect complex, relax flat
                local_var = float(region.astype(np.float32).var())
                if local_var > self.local_var_high:
                    base_qp -= self.qp_delta_protect
                elif local_var < self.local_var_low:
                    base_qp += self.qp_delta_relax

                # Motion intensity: relax QP on fast-moving blocks
                motion_region = self.motion_map[y0:y1, x0:x1]
                mean_motion = float(motion_region.mean())
                if mean_motion > self.motion_high_thresh:
                    base_qp += self.qp_delta_relax

                self.qp_map[cy, cx] = base_qp

        # --- Transition zone expansion: dilate ROI by DILATION_CTU CTUs ---
        temp_qp = self.qp_map.copy()
        for cy in range(self.map_h):
            for cx in range(self.map_w):
                if self.qp_map[cy, cx] == roi_qp:
                    continue
                y0 = max(0, cy - DILATION_CTU)
                y1 = min(self.map_h, cy + DILATION_CTU + 1)
                x0 = max(0, cx - DILATION_CTU)
                x1 = min(self.map_w, cx + DILATION_CTU + 1)
                neighborhood = temp_qp[y0:y1, x0:x1]
                if (neighborhood == roi_qp).any():
                    if self.qp_map[cy, cx] == bg_qp:
                        self.qp_map[cy, cx] = trans_qp

        # --- Night mode hard ceiling (applied last) ---
        if self.night_mode:
            self.qp_map = np.clip(self.qp_map, None, night_clamp)

    def _suggest_gop(self):
        """Suggest GOP size based on scene dynamics."""
        avg_fg = np.mean(list(self.fg_history)) if self.fg_history else 0.0

        if avg_fg < 0.01 and self.consecutive_static > 150:
            return self.static_gop_size
        elif avg_fg > 0.15:
            return self.dynamic_gop_size
        elif avg_fg > 0.05:
            return self.normal_gop_size
        else:
            return self.normal_gop_size

    def should_force_idr(self):
        """Determine if an IDR frame should be forced for this frame."""
        if self.frame_index == 0:
            return False
        if self.idr_cooldown > 0:
            return False
        return self.motion_onset

    def get_recommended_frame_type(self):
        """Get the recommended frame type for the current frame."""
        if self.frame_index == 0:
            return 'I'
        if self.idr_pending:
            return 'I'
        gop = max(self.current_gop_size, 1)
        pos = self.frame_index % gop
        if pos == 0:
            return 'I'
        elif pos % 5 == 0:
            return 'P'
        else:
            return 'b'

    def set_gop_params(self, static_gop, dynamic_gop, normal_gop):
        """Configure GOP size parameters."""
        self.static_gop_size = static_gop
        self.dynamic_gop_size = dynamic_gop
        self.normal_gop_size = normal_gop

    def reset(self):
        """Reset background model (after scene cut)."""
        self.vibe = ViBeModel(self.ds_width, self.ds_height)
        self.tracker = ObjectTracker()
        self.fg_history.clear()
        self.consecutive_static = 0
        self.motion_onset = False
        self.idr_cooldown = 0
        self.idr_pending = False
        self.current_gop_size = self.normal_gop_size
        self.frame_index = 0
        # v2: reset smoothing state
        self.prev_qp_map = None
        self.prev_ds_y = None
