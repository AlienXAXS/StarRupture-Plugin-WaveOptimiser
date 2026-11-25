# Update notes

## Unreleased

- A feature the server host switches off (or the loader turns off) now stops acting right away. Rupture heat batching hands new wave calls to the game unchanged once it has applied what was already queued, so no entity is lost. The heat tools ignore new requests and drop a pending heat reset instead of running it later.
- Shutdown removes the hooks the plugin installed, even if a feature was switched off in the meantime.
- Loads on ModLoader v1.23.0 and later (it declares the interface level it needs instead of the newest).
- Every game address and member offset comes from the loader's version object map. The plugin carries no built-in patterns or offsets, so a game build the version map has no row for turns off the feature that needs it.
