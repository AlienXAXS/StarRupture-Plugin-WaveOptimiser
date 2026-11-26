# Update notes

## Unreleased

- Shutdown removes the hooks the plugin installed, whichever features are running.
- Loads on ModLoader v1.23.0 and later (it declares the interface level it needs instead of the newest).
- Every game address and member offset comes from the loader's version object map. The plugin carries no built-in patterns, so a game build the version map has no row for turns off the feature that needs it.
- You can now install and update it from the ModLoader's Browse list.
