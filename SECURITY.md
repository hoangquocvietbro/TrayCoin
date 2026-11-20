# Security

If you find a security issue, please report it privately through the GitHub Security tab ("Report a vulnerability") on https://github.com/hoangquocvietbro/traycoin.

## Downloads

Only download `TrayCoin.exe` from the official releases page of this repository:
https://github.com/hoangquocvietbro/traycoin/releases

Every release is built cleanly by GitHub Actions directly from source. You can verify your binary:

- Compare its SHA-256 checksum with `TrayCoin.exe.sha256` on the release page, or
- Verify build provenance with GitHub CLI:
  `gh attestation verify TrayCoin.exe --repo hoangquocvietbro/traycoin`

## Privileges & Network

- **No administrator privileges required:** TrayCoin runs entirely in standard user mode (autostart uses `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- **No kernel drivers or external runtimes:** All hardware-driver code (PawnIO) from the upstream TraySense project has been removed.
- **Read-only market data:** TrayCoin only makes HTTPS / WSS connections to public cryptocurrency exchange, DEX, and aggregator APIs to fetch prices; it never handles wallets, private keys, or trading API keys.
