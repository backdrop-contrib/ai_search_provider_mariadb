# AI Search MariaDB Vector — Dev Notes

Vector storage backend for the `ai_search` framework (see `../ai_search/CLAUDE.md` for the overall architecture). This module is its own git repo/project (`backdrop-contrib/ai_search_provider_mariadb`), a sibling of `ai_search`, not a submodule.

See `README.md` in this directory for requirements, the auto-created table schema, and configuration options — don't duplicate that here.

## Code Map

- `ai_search_mariadb.module` — `hook_autoload_info()`, `hook_search_api_service_info()` registering the `mariadb_vector` backend
- `includes/MariadbVectorService.inc` — `MariadbVectorService extends SearchApiAbstractService implements AiSearchBackendInterface`. Talks to MariaDB directly via `db_query()` (no separate vector client class, no custom DB driver) — it uses Backdrop's normal DB layer plus MariaDB-only SQL (`VEC_FromText()`, `VEC_DISTANCE_COSINE()`/`VEC_DISTANCE_EUCLIDEAN()`, `VECTOR INDEX`).

## Known Constraints

- Requires MariaDB 11.7+ (11.8 LTS recommended); **does not work on MySQL** — the whole point of this module is MariaDB's native `VECTOR` type.
- Metric (`COSINE`/`L2`) is fixed at table creation time (baked into the `VECTOR INDEX`); changing it requires dropping/rebuilding the table and re-indexing.
- `deleteItems()` accepts raw IDs (using the index's entity type) and canonical `type/id` IDs for non-node entity types.

**Last Updated:** 2026-07-07 by Claude
