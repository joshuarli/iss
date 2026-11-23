# AGENTS.md

iss is a macOS command that replaces the Space-switch animation by rewriting trackpad gesture events. All source is in `iss.c`.

- Run `make lint` for cppcheck. Install cppcheck first.
- Run `make test` to compile `iss.c` with warnings as errors. Gesture behavior needs a logged-in macOS session and has no automated test.
- Do not run a formatter. The fleet bans formatters.
