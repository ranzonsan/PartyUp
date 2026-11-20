# PartyUp

PartyUp is a lightweight, self-hosted desktop app for small groups — voice, text and screen sharing.

## Features

- **Lightweight and easy to get started**: built with .NET 10 + Avalonia; a clean, responsive UI that stays light on resources.
- **Everything in one place**: voice, video and text.
- **A simple permission model**: just three roles — owner, admin and user — with none of the tedious configuration.
- **Quick to deploy**: the server bundles the control plane, Pion SFU, STUN and SQLite storage. A single command or a single executable keeps it running, with no extra services.
- **Low server load**: peer-to-peer (end-to-end) voice and video are coming, keeping as much traffic off your server as possible.

## Quick start

### 1. Get the client (Windows)

Download the latest build from [Releases](https://github.com/ranzonsan/partyup/releases/latest):

- `PartyUp-<version>-win64-Setup.exe`: recommended for most people. Supports auto-update.
- `PartyUp-<version>-win64.msi`: for a specific install path or for deployment through software distribution.
- `PartyUp-<version>-win64-portable.zip`: portable, no installation, no auto-update.

On first launch, enter the server address and register with an invite code from the admin.

### 2. Deploy the server (your own machine)

Your server needs:

- At least 512 MB of free storage and 256 MB of free memory.
- At least one port that your users can reach.

PartyUp ships the server as a Docker image. Start one with:

```bash
docker run -d --name partyup --network host --restart unless-stopped \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest
```

Once it is running, read the bootstrap token from the logs:

```bash
docker logs partyup | head -20
```

Use the bootstrap token to register the owner account for this server.

> [!CAUTION]
> Keep the bootstrap token safe until the owner account exists. Anyone who can reach the server can use this token to register the owner account and take over the server.

## Known limitations

- The client is Windows-only.
- Installers are unsigned, so Windows SmartScreen warns about an unknown publisher.
- End-to-end (peer-to-peer) media is still under development.

## Documentation

| Doc | Contents |
|---|---|
| [Product](docs/PRODUCT.md) | goals, non-goals, transport strategy, v1 acceptance |
| [Architecture](docs/ARCHITECTURE.md) | components, C ABI boundary, data flow, path state machine |
| [Protocol](docs/PROTOCOL.md) | signaling message shapes |
| [Docker deployment](docs/DOCKER.md) | containerization, host vs fixed-port networking, troubleshooting |
| [Packaging & distribution](docs/PACKAGING.md) | portable / Setup / MSI / server bundle, version source, SmartScreen |
| [Development](docs/DEV.md) | toolchain baseline, build flow, known pitfalls |
| [Glossary](GLOSSARY.md) | project vocabulary; authoritative for prose and code naming |
| [Project boundaries](docs/BOUNDARIES.md) | scope / architecture / collaboration / legal — read this before proposing anything |
| [Documentation index](docs/README.md) | find what to read by role (user / operator / contributor / agent) |
| [Architecture decisions](docs/adr/) | irreversible decisions and their rationale |
| [Research: desktop GUI and media stack](docs/research/2026-11-01-desktop-media-stack.md) | selection facts and sources |
| [Brand assets](brand/) | icons, splash, horizontal/vertical lockups |

Verification notes and measured evidence (screenshots, candidate addresses, temporary tokens) live in `.docs/` and are **not committed** — they contain internal addresses and machine details, and are process material rather than documentation.

## Building from source

```bash
# server (Go 1.26+)
cd server && go build ./cmd/partyup

# client (.NET 10 + Avalonia)
dotnet build client/PartyUp.Client.csproj -c Release

# native media library (needs MSVC + prebuilt libwebrtc, see docs/DEV.md)
pwsh -File scripts/build-media-dll.ps1
```

CI runs entirely on GitHub Actions:

| Workflow | Trigger | Output |
|---|---|---|
| `ci.yml` | push / PR | Go tests + client build |
| `media-core.yml` | `src/media-core/**` or manual | `partyup_media.dll` → `media-core-v*` release |
| `docker.yml` | `server-v*` tag / `main` | server image → `ghcr.io/ranzonsan/partyup` |
| `release-client.yml` | `v*` tag | client bundles → draft Release (marked Latest) |
| `release-server.yml` | `server-v*` tag | server zip → draft Release (not Latest) |

## Contributing

- **[Contributing guide](CONTRIBUTING.md)** (Chinese): what gets accepted, how to open a PR, how to verify it. Read the "won't be accepted" table first.
- **[Project boundaries](docs/BOUNDARIES.md)** (Chinese): scope / architecture / collaboration / legal. If you are unsure whether something belongs here, start here.
- **[Code of conduct](CODE_OF_CONDUCT.md)** (Chinese; canonical English text at contributor-covenant.org): the precondition for taking part in issues, PRs and discussions.
- **[Security policy](SECURITY.md)**: report vulnerabilities through a private channel, never in a public issue.
- **[Governance](GOVERNANCE.md)** and **[name policy](TRADEMARK.md)**: who decides and how releases work; and how forks must rename.

AI-assisted contributions are welcome, but they must disclose the workflow and include verification that was actually run.

## License

PartyUp is licensed under [GPL-3.0](LICENSE).
