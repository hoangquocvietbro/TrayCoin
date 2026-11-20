# Third-Party Components

TrayCoin is a standalone pure Win32 C application with **zero bundled third-party binaries, libraries, or kernel drivers**.

- All PawnIO kernel driver modules from the upstream TraySense project have been removed.
- The binary links exclusively against built-in Windows system libraries (`user32`, `gdi32`, `shell32`, `advapi32`, `ole32`, `oleaut32`, `comctl32`, `comdlg32`, `winhttp`, and dynamically loaded `iphlpapi.dll` from System32).
- Market prices are fetched directly over HTTPS / WSS from public exchange, DEX, and aggregator APIs selected by the user.
- See [LICENSE](LICENSE) and [NOTICE](NOTICE) for full GNU AGPL-3.0 and upstream TraySense attribution details.
