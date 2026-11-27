# Long Road sandbox to-do

> **Feature retention rule. Existing features must never be removed, disabled, or hidden without the repository owner's express permission. Backward compatibility is a separate decision for the stable version.**

## First playable systems

- [x] Separate ambience and vehicle audio controls. Reduce repetitive cabin noise and suppress unloaded or stalled engine loops.
- [x] Persist male/female character selection, tops, and headwear. Equip and remove clothing through the pack.
- [x] Add procedural cowboy hats, jackets, and dresses.
- [x] Craft and place individual floor panels, walls, doorways, doors, and roof panels.
- [x] Open, close, and paint player-built doors. Reclaim placed parts into the pack and replace them individually.
- [x] Add basic soil plots, a seed crop, watering, growth, and repeatable harvests.
- [x] Persist placed parts, plants, and outfits in the trip save.

## Editable procedural buildings

- [ ] Give each generated building, room, doorway, window, floor, wall, roof, and fixture a stable component ID derived from its seed, lot, and component index.
- [ ] Convert static geometry and collision into editable components and rebuild only the affected render chunks.
- [ ] Generate usable interiors and consistent entrances for every building, including towers, warehouses, homes, roadside stops, and utility sheds.
- [ ] Generate hallways, stairs, landings, floor access, balconies, and exits.
- [ ] Make existing doors openable, closable, removable, replaceable, and decoratable.
- [ ] Add hinges, locks, keys, handles, paint, signage, window inserts, and front/back orientation.
- [ ] Salvage walls, doors, trim, floors, stairs, windows, roofing, insulation, and fasteners into inventory.
- [ ] Support removing every component on a lot and clearing it for rebuilding and farming.
- [ ] Add material strength, damage, repair, demolition, recoverable scrap, and collapse rules.
- [ ] Add grid and free placement, snap points, elevation controls, alignment, overlap validation, and previews for all components.
- [ ] Add beams, posts, room corners, siding, floors, foundations, tiles, stair kits, fences, gates, and sheds.
- [ ] Add wood, brick, steel, canvas, and glass styles with procedural paint and decorations.
- [ ] Preserve demolition, landscaping, and edits through streaming, reloading, and version migrations.

## Carrying, storage, and inventory

- [x] Pick up previously placed items directly with Use.
- [ ] Hold objects them in hand, rotate them, and place them on shelves, counters, walls, or vehicle interiors.
- [ ] Add size-aware collision, surface snapping, stacking, gravity, and stable behavior while a vehicle moves.
- [ ] Support crates, cabinets, drawers, shelves, coolers, trunks, backpacks, and nested contents.
- [ ] Add search, sort, split, merge, capacity by mass and volume, and quick transfer.
- [ ] Add procedural item models and readable materials for every component, tool, seed, garment, and decoration.
- [ ] Prevent duplication and loss when inventories are full, saves reload, or placement is canceled.

## Land and gardening

- [ ] Clear rubble and foundations, grade soil, till arbitrary terrain, and restore previous land use.
- [ ] Add raised beds, rows, pots, orchards, trellises, fences, greenhouses, and compost bins.
- [ ] Grow tomatoes, potatoes, carrots, corn, beans, herbs, berries, flowers, and fruit trees.
- [ ] Track moisture, temperature, light, soil quality, pests, seasons, seeds, and pollination.
- [ ] Add water collection, irrigation, fertilizer, compost, tools, seed saving, and storage.
- [ ] Integrate harvesting with cooking, trading, preservation, and autonomous demo behavior.

## Characters and outfits

- [ ] Add character creation with skin tones, hair, body presets, names, and a 3D preview.
- [ ] Expand wardrobe slots for tops, bottoms, dresses, outerwear, shoes, hats, accessories, and backpacks.
- [ ] Add patterned dresses, skirts, shirts, jeans, leather jackets, boots, cowboy hats, gloves, and jewelry.
- [ ] Add procedural stitching, fabric and leather materials, colors, wear, warmth, wetness, and repair.
- [ ] Fit garment meshes across both bodies and prevent clipping in walking, driving, and crouching poses.

## Audio and validation

- [ ] Tune cabin, road, tires, weather, wildlife, and machinery as distinct procedural sound layers.
- [ ] Test vehicle audio on Android and check repeated pause, resume, enter, exit, and demo transitions.
- [ ] Stress-test full lots and inventories, older saves, repeated city rebuilds, and low-memory devices.
- [ ] Add demo scenarios for buying parts, building shelter, gardening, harvesting, salvaging, and changing outfits.
- [ ] Keep procedural meshes, textures, and sounds compact. Measure APK size and runtime memory at each release.
