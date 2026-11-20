# TrayCoin — Taskbar & Widget Crypto Tracker

Lightweight, pure Win32 C cryptocurrency & meme coin price tracker for Windows 10/11 — featuring a native Taskbar overlay, an Always-on-Top glass Floating Widget, Real-Time WebSockets, and a parallel multi-exchange/DEX engine.

![TrayCoin Taskbar Overlay, Floating Widget & Settings](asset/Annotation%202026-11-20%20105634.png)

![TrayCoin Watchlist Manager (50 Coins Live WebSocket)](asset/Annotation%202026-11-20%20105843.png)

```text
[↻]  BTC $63,855 -0.9%   ETH $1,804.3 -0.1%   PEPE $0.0₅382 +2.2%
```

## Key Features

- **Ultra-Light Native Win32 C:** Single ~350 KB `.exe`, zero external runtime, ~8 MB RAM, instant startup, no administrator rights required.
- **16+ Exchange, DEX & Aggregator APIs:**
  - **CEX (No API Key):** Binance (Spot + Futures + fallback mirror), OKX, Bybit, Gate.io, MEXC, KuCoin, Bitget, Coinbase, Kraken, HTX (Huobi).
  - **DEX & Meme Coins (No API Key):** **DexScreener** (Token Contract, Pair Address, URL, or Name Search across Solana, Ethereum, Base, BSC, Arbitrum, TON, Sui...), **GeckoTerminal**, **Jupiter** (Solana).
  - **Aggregators:** **CoinGecko** (Free / optional Demo key), **CoinMarketCap** (optional free Pro API key), plus **Auto (Smart)** routing.
- **Real-Time WebSocket + Parallel Fetch Pool:**
  - Native WinHTTP WebSocket streaming for **Binance Spot, Binance Futures, OKX, Bybit, Gate.io** with 250 ms UI throttling, 45 s zombie-socket watchdog, instant reconnect on Sleep/Resume or Wi-Fi/VPN changes (`NotifyIpInterfaceChange`), and automatic HTTP fallback.
  - 6-worker parallel HTTP/2 fetch pool with provider batching (up to 64 coins), custom per-coin/global refresh intervals (`2`–`3600`s), and HTTP 429 exponential backoff.
- **Always-on-Top Floating Widget & Global Hotkeys:**
  - Windows 11 glass card (**Vertical Card** with 24-point mini sparklines, portfolio footer & automatic multi-column expansion when full height, or **Horizontal Pill** with Lock/Settings icons & automatic multi-row wrapping), adjustable **Opacity (30%–100%)**, magnetic edge snapping, **Lock & Click-Through** (`WS_EX_TRANSPARENT`).
  - Global hotkeys: `Ctrl+Shift+C` (Show/Hide Widget), `Ctrl+Shift+L` (Toggle Click-Through lock), `Ctrl+Shift+R` (Refresh Now).
- **Smooth Taskbar UX:**
  - Clickable animated **`↻` Refresh icon**, **green/red 500 ms Tick Flash**, automatic **Pagination + Mouse-Wheel scroll** + optional **Auto-Rotate** when tracking many coins.
- **Watchlist Manager & Alerts:**
  - Resizable/maximizable 2-Tab native Windows Settings window with **instant offline Search/Filter**, **Select/Unselect All**, **multi-select right-click menu** (selective refresh, show/hide, move, copy contract, delete), **Price Alerts** (Windows notifications + sound), **Portfolio Holdings**, **Export/Import JSON**, and toggleable **Debug Logging (`traycoin.log`)** + crash handler.

## Building

```bat
build.bat
```
*(Uses Visual Studio MSVC or MinGW-w64; pass `build.bat msvc` or `build.bat mingw` explicitly, or `./build.sh` on Linux/MSYS2.)*

## Diagnostics & Live Tests

- `TrayCoin.exe --test-api` — Verify all 16+ CEX/DEX/Aggregator APIs live.
- `TrayCoin.exe --test-poller` — Benchmark parallel HTTP polling on your `config.json`.
- `TrayCoin.exe --test-ws` — Verify live WebSocket streams (Binance Spot/Futures, OKX, Bybit, Gate.io).
- `TrayCoin.exe --test-refresh` — End-to-end multi-refresh regression test with WebSockets active.

## Author, Modifications & License

- **Author of TrayCoin modifications & features:** [`@hoangquocvietbro`](https://github.com/hoangquocvietbro) (2026)
- **Original upstream project:** Based on **TraySense** (Copyright © 2026 DMTWEAKS), whose Win32 taskbar overlay and UI Automation placement architecture served as the starting foundation.
- **License:** Licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** — see [LICENSE](LICENSE) and [NOTICE](NOTICE) for full copyright notices and the complete summary of modifications in accordance with AGPL-3.0 Section 5.
