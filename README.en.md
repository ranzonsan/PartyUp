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
- At least two ports that your users can reach: one for the server's main communication, the other for voice transport.

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
- Builds are not code-signed, so Windows SmartScreen warns about an unknown publisher; every release ships a `SHA256SUMS.txt` you can verify against.
- End-to-end (peer-to-peer) media is still under development.

## Documentation

Usage documentation lives in the [Wiki](https://github.com/ranzonsan/PartyUp/wiki).
Documentation for developing the project lives in [Docs](docs).

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

## Contributing

- **[Contributing guide](CONTRIBUTING.md)**: how to share code, documentation or other content with this project, and where the limits are.
- **[Project boundaries](docs/BOUNDARIES.md)**: scope, architecture, collaboration and legal — the four parts that determine whether a (new) requirement is viable.
- **[Code of conduct](CODE_OF_CONDUCT.md)**: the rules that must be followed when taking part in issues, PRs and discussions.
- **[Security policy](SECURITY.md)**: how vulnerabilities are reported. Use a private channel; do not post details in a public issue.
- **[Governance](GOVERNANCE.md)**: who decides, the ADR process and the release process; **[name policy](TRADEMARK.md)**: how the project's name and icons may be used.

The project allows and welcomes AI-generated code, provided it is produced within these rules.

## License

PartyUp is licensed under [GPL-3.0](LICENSE).
