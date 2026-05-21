# Security Policy

x265-optimizer launches external tools such as FFmpeg, x265, and GPU utilities. Treat all user-provided file paths and media files as untrusted input.

## Reporting a vulnerability

Please open a private security advisory on GitHub if available, or contact the maintainer directly before publishing details.

Include:

- A minimal reproduction.
- The affected command or API.
- OS, Python, FFmpeg, and driver versions.
- Whether the issue requires a malicious media file or only command-line input.

## Scope

In scope:

- Unsafe command construction.
- Path handling issues.
- Unexpected execution through crafted filenames or config values.
- Unsafe parsing of benchmark logs or config files.

Out of scope:

- Vulnerabilities in FFmpeg, x265, GPU drivers, or vendor runtimes unless this project makes exploitation materially easier.
