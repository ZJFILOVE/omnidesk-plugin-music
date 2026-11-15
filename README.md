# Apple Music (Adam FairPlay Lossless) (`plugin-music`)

Standalone OmniDesk C++17 `.so` workspace plugin for Apple Music powered by the headless Adam Go engine (`/home/zhu/Projects/apple-music-online/engine`) and Android FairPlay ALAC decryption (`libdrm_client.so`).

## Capabilities

- **Provides**: `media.source.v1`, `media.lyrics.v1`, `IMediaSource`, `WorkspaceTabSpec` (`Apple Music`), and `DockWidgetSpec` (`F8` Companion Dock)
- **Consumes**: `llm.translate.v1`, `llm.chat.v1`, `ai.agent.v1`, `web.cookies.v1` (optional)

## Features

- **Headless Adam Go Engine Lifecycle**: Reuses an existing `127.0.0.1:20025` engine or spawns `engine --api 20025` headlessly without Electron, using loopback UID verification and leaf-certificate-pinned TLS.
- **Native Apple ID & 2FA Authentication**: Supports `POST /api/v1/drm/authenticate`, `POST /api/v1/drm/challenge`, `POST /api/v1/drm/challenge/resend`, plus persistent FairPlay session reuse and optional `media-user-token` import.
- **24bit/192kHz ALAC & MV Playback**: Creates FairPlay playback sessions (`POST /api/v1/playback`) and streams decrypted lossless audio or MV segments directly into OmniDesk `MPVCore`.
- **Catalog, Recommendations & Library Playlists**: Browses personalised feeds (`recommendations`, `heavy-rotation`, `recently-played`), local SQLite playlists, album/playlist/artist details, and Apple Music Catalog Search.
- **Bilingual Lyrics & AI Explanation**: Fetches native Apple Music LRC (`/api/v1/lyrics/{id}` with storefront fallback), falls back to exact-signature LRCLIB lookup, translates missing lines via `llm.translate.v1` / `llm.chat.v1`, caches `original.lrc` / `zh-CN.lrc` / `bilingual.lrc`, and provides phrase-level AI explanations.

## Build & Install

```bash
omnidesk-pm install .
omnidesk-pm verify plugin-music
```
