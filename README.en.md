# PartyUp

> Self-hosted, channel-based desktop app for **text, voice and screen sharing** — built for small groups (2–12 people): gaming nights, watching things together, remote collaboration.
> The server is **a single binary** that runs on your own machine. No cloud services involved.

## Why

- **One command to run a server**: `docker run` or a single exe. No livekit-server, no separate TURN, no database server to install.
- **Voice + screen sharing**: join voice and share your screen inside a channel; the transport mode is decided by the server (`relay` saves client bandwidth / `direct` saves server bandwidth) and the client doesn't have to care.
- **Your data stays yours**: accounts, channels and chat history live in SQLite on your machine. No federation, no external identity provider, no telemetry.
- **Admin panel**: kick/ban members, invite codes, audit log, announcement & welcome message, transport modes and concurrency limits, with owner/admin roles.
- **Install once, updates itself**: the Windows client checks for new versions, asks before downloading, and applies on restart.

## Quick start

### 1. Run the server (Linux, ~30s)

```bash
docker run -d --name partyup --network host --restart unless-stopped \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest

docker logs partyup | head -20
# BOOTSTRAP_TOKEN=xxxx   ← used once to create the owner account
# INVITE_CODE=xxxx       ← share with friends to register
```

Then open `http://<server-ip>:8080` and use `BOOTSTRAP_TOKEN` to create your admin account.

> ⚠️ **`--network host` is the simplest setup**: voice/screen media uses UDP on dynamic ports, so it does not work behind Docker's bridge network (text still works — easy to misdiagnose).
> For bridge networking, pass `--ice-port-range 50000-50010 --ice-public-ip <public-ip>` and publish the same UDP range with `-p 50000-50010:50000-50010/udp` (see [Docker deployment](docs/DOCKER.md)).
> Docker Desktop on Windows/macOS does not provide real host networking and **cannot** be used as the server; run the binary directly there.

**No Docker?** Download `PartyUp-Server-<version>-win64.zip` and run `start.cmd`; see [Packaging](docs/PACKAGING.md) for other platforms.

### 2. Install the client (Windows)

Download any asset from [Releases](https://github.com/ranzonsan/partyup/releases):

| File | For whom |
|---|---|
| `PartyUp-<version>-win64-Setup.exe` | Most people: one-click, per-user install, **supports auto-update** |
| `PartyUp-<version>-win64.msi` | Managed software distribution / choosing install scope |
| `PartyUp-<version>-win64-portable.zip` | Portable: unzip and run, no auto-update |

On first launch enter the server address (e.g. `http://192.168.1.10:8080`); ask the admin for an invite code.

## How it fits together

```
┌────────────┐   HTTP / WebSocket (control plane: login, channels, text, signaling)  ┌──────────────────────┐
│  Windows   │ ────────────────────────────────────────────────────────────────────> │ Go single binary     │
│  client    │                                                                       │  · control plane     │
│ (Avalonia) │ <──────────────────── UDP (voice / screen media, in-process SFU) ────── │  · Pion SFU in-proc  │
└────────────┘                                                                       │  · SQLite            │
       │                                                                             └──────────────────────┘
       └─ partyup_media.dll (C++ / libwebrtc, narrow C ABI)
```

See [Architecture](docs/ARCHITECTURE.md) and [Protocol](docs/PROTOCOL.md); irreversible decisions live in [ADR](docs/adr/).

## Requirements

| Item | Guideline |
|---|---|
| Server | 1 vCPU / 512 MB to start; relay bandwidth grows with concurrent voice and screen shares |
| Network | control plane TCP 8080; media UDP (dynamic ports, or a configured fixed range) + optional STUN UDP 3478 |
| Client | Windows 10/11 x64 with network access to the server |

Measured numbers: [Capacity](docs/CAPACITY.md).

## Documentation

| Doc | Contents |
|---|---|
| [Product](docs/PRODUCT.md) | goals, non-goals, transport strategy, v1 acceptance |
| [Architecture](docs/ARCHITECTURE.md) | components, C ABI boundary, data flow, session states |
| [Protocol](docs/PROTOCOL.md) | signaling message shapes |
| [Docker](docs/DOCKER.md) | container deployment, host vs fixed-port networking, troubleshooting |
| [Packaging](docs/PACKAGING.md) | portable / Setup / MSI / server bundle, version source, SmartScreen |
| [Development](docs/DEV.md) | toolchain baseline, build flow, known pitfalls |
| [Glossary](GLOSSARY.md) | project vocabulary (Chinese; source of truth for naming) |
| [ADR](docs/adr/) | irreversible decisions and their rationale |
| [Research: desktop GUI and media stack](docs/research/2026-11-01-desktop-media-stack.md) | selection facts and sources |
| [Brand assets](brand/) | icons, splash, horizontal/vertical lockups |

Verification notes and measured evidence (screenshots, ICE candidates, temporary tokens) live in `.docs/` and are **not committed** — they contain internal addresses and machine details, and are process artefacts rather than documentation.

## Building from source

```bash
# server (Go 1.26+)
cd server && go build ./cmd/partyup

# client (.NET 10 + Avalonia)
dotnet build client/PartyUp.Client.csproj -c Release

# native media library (needs MSVC + prebuilt libwebrtc, see docs/DEV.md)
pwsh -File scripts/build-media-dll.ps1
```

CI runs on GitHub Actions:

| Workflow | Trigger | Output |
|---|---|---|
| `ci.yml` | push / PR | Go tests + client build |
| `media-core.yml` | `src/media-core/**` or manual | `partyup_media.dll` → `media-core-v*` release |
| `docker.yml` | `server-v*` tag / `main` | server image → `ghcr.io/ranzonsan/partyup` |
| `release-client.yml` | `v*` tag | client bundles → draft Release (marked Latest) |
| `release-server.yml` | `server-v*` tag | server zip → draft Release (not Latest) |

Client and server have **independent versions and release channels**: client `client/VERSION`
↔ tag `v0.1.0`, server `server/VERSION` ↔ tag `server-v0.1.0`.

## Known limitations

- The client is **Windows-only** for now (architecture is cross-platform; Linux/macOS ports are not done).
- Installers are **unsigned**, so Windows SmartScreen warns about an unknown publisher.
- No camera, no end-to-end encryption; text messages are relayed and stored without backup guarantees.
- Screen sharing in `direct` transport mode is 1-to-1 only.
- The server container is best run with host networking; bridge networking needs a fixed port range plus a public-IP override (see [Docker](docs/DOCKER.md)).

## Status

v1 is feature-complete and passes seven acceptance areas (packaging, invite codes, text history persistence, voice, screen sharing, both transport paths, reconnect). See [Product](docs/PRODUCT.md).

## License

[GPL-3.0](LICENSE).
