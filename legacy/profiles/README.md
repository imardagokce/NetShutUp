# profiles/

This directory will hold the built-in profile definitions for NetShutUp Legacy.

Each profile is a JSON file that maps setting IDs (from the catalog) to their
target values. Profiles are embedded into the binary at compile time — no
external files are needed at runtime.

## Planned files

| File | Profile |
|---|---|
| `privacy.json` | Privacy profile |
| `performance.json` | Performance profile |
| `recommended.json` | Recommended profile |

Profile format and schema will be defined once the settings catalog module
(`src/catalog.h`) is implemented.

> **Note:** Every setting ID in a profile must exist in the catalog.
> A profile may only assign values to catalog-defined IDs — no freeform
> registry paths are permitted.
