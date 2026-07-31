![Logo of Kastenbrot](https://raw.githubusercontent.com/Big-Onche/Cube-Craft/c662889f6539805b216470690c0b74994381be6b/media/interface/logo.png) 

**Kastenbrot** is an experimental voxel sandbox built on Cube Engine.

It combines Cube’s fast octree editing, responsive multiplayer networking, and real-time rendering with streamed procedural worlds, persistent world modification, and unrestricted creative building.

The project is based on [Tesseract](http://tesseract.gg/), a fork of Cube 2: Sauerbraten. Kastenbrot substantially replaces and extends its world, gameplay, networking, and persistence systems to support large seed-based voxel environments.

> **[!IMPORTANT]**
> Kastenbrot is a work in progress. Exploration, procedural generation, block interaction, multiplayer, and core building systems are playable, but survival progression, menus, content, balancing, and general polish remain under active development.
>
> **Save compatibility may change while world generation and persistence formats evolve.**

> **[!WARNING]**
> Large parts of the project were prototyped rapidly with OpenAI Codex. Some generated code still requires manual review, cleanup, and refactoring. Expect occasional cursed archaeology in the source tree.

---

## 🚧 Roadmap and Stability
- 🟩🟩🟩🟩⬛ **Stability** : Mostly stable
- 🟩🟩🟩⬛⬛ **Performance** : Acceptable
- 🟩🟩🟩🟩⬛ **Multiplayer** : Simple yet working
- 🟩⬛⬛⬛⬛ **Content:** Extremly early
- 🟩⬛⬛⬛⬛ **Survival:** Extremly early
- 🟩🟩⬛⬛⬛ **UI:** Very early
- ⬛⬛⬛⬛⬛ **Accessiblity:** No accessibility settings
- ⬛⬛⬛⬛⬛ **Sound**: No sound
- 🟩🟩🟩🟩🟩 **Stupidity of the project**: Maximum

Most voxel games treat the world as a collection of fixed blocks and save every generated region to disk.

Kastenbrot takes a different approach:

- The world is generated deterministically from a seed.
- Untouched terrain is never stored.
- Only player modifications are saved as compact chunk diffs.
- Cube Engine’s octree allows terrain to be edited at multiple resolutions.
- Multiplayer clients generate the same base world locally.
- The server only replicates authoritative changes.

The result is a sandbox designed around fast exploration, powerful world editing, compact persistence, and responsive multiplayer.

---

## Features

### Procedural worlds

- Large, seed-based worlds streamed dynamically in chunks
- Multithreaded, priority-based chunk generation and loading
- Procedural continents, oceans, islands, mountains, caves, and terrain variation
- Deterministic generation shared between clients and servers
- Key-based multiplayer authentification
- Dynamic day and night cycle
- Six-direction skylight propagation and ambient sky exposure

### Rendering

Kastenbrot inherits and extends Tesseract’s modern deferred renderer:

- Fully dynamic lighting
- Dynamic shadow mapping
- High dynamic range rendering
- Bloom
- Screen-space ambient occlusion
- Screen-space reflections and refractions
- Volumetric lighting
- Atmospheric scattering
- Frustum and occlusion culling
- Level-of-detail and chunk-streaming optimizations

### Building and world editing

- Block placement and destruction
- Native octree-based world geometry
- Multiple editing resolutions
- Cube Engine-style real-time edit mode
- Large volume selection, copy, paste, rotation, and material editing
- Foundations for carved blocks, custom geometry, and prefabs
- Background saving and automatic diff compaction
- Persistent undo, redo, rollback, and corruption recovery

### Multiplayer

- ENet-based networking
- Fast-paced movement and client prediction inherited from Cube 2
- Responsive player movement under latency
- Real-time authoritative world updates
- Cooperative multiplayer map editing
- Server-authoritative block placement and destruction
- Per-chunk revisions and client/server hash verification
- Recovery from missing or mismatched chunk diffs
- Separate replication paths for movement, gameplay events, and world data

## Credits

Kastenbrot builds upon the work of:
- Cube 2: Sauerbraten
- Tesseract
- ENet
- SDL2
- FastNoiseLite