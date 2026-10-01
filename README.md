# AI Search MariaDB Vector

MariaDB native `VECTOR` provider for the Backdrop **AI Search** framework. Stores
embeddings directly in the site database and runs nearest-neighbor search with
MariaDB's built-in HNSW vector index — no external vector database (Milvus),
no PostgreSQL, and no custom database driver.

This is a port of the `ai_search_provider_pgvector` backend. Because MariaDB is
Backdrop's native engine, `db_query()` talks to it directly, so this provider
only depends on `ai_search`.

## Requirements

- **MariaDB 11.7+** (11.8 LTS recommended — vector search is GA there).
- The site's default database connection (or a dedicated one) must point at a
  qualifying MariaDB server. **MySQL does not work** — the `VECTOR` type,
  `VEC_FromText()`, `VEC_DISTANCE_*()` and `VECTOR INDEX` are MariaDB-only.
- `ai_search` module enabled and configured with an embeddings engine.

## Installation

- Install this module using the official [Backdrop CMS instructions](https://backdropcms.org/user-guide/modules).

## How it works

Per Search API index, the provider auto-creates a table:

```sql
CREATE TABLE `search_vector_<index>` (
  id VARCHAR(200) NOT NULL PRIMARY KEY,
  backdrop_entity_id VARCHAR(200),
  nid INT NULL,
  content LONGTEXT,
  vector VECTOR(<dim>) NOT NULL,
  VECTOR INDEX (vector) M=<m> DISTANCE=<cosine|euclidean>
) ENGINE=InnoDB;
```

- **Index:** `INSERT ... VALUES (..., VEC_FromText('[...]')) ON DUPLICATE KEY UPDATE ...`
- **Search:** `ORDER BY VEC_DISTANCE_COSINE(vector, VEC_FromText(:q)) LIMIT k`

## Configuration

`admin/config/search/search_api` → add a server → choose **MariaDB Vector**.

- **Metric type:** `COSINE` or `L2` (euclidean). **Fixed at table creation** —
  MariaDB bakes the metric into the `VECTOR INDEX`. To change it, drop/rebuild
  the table (re-index).
- **HNSW M:** graph connectivity. Higher = better recall, more storage. Default 6.
- **Table name:** optional override; otherwise `search_vector_<index machine name>`. A custom table name must be unique per Search API index.

## Known limitations

- Metric cannot be changed in place (see above).
- Only `COSINE` and `L2` are supported (MariaDB has no inner-product distance fn).
- `deleteItems()` accepts raw IDs and canonical IDs for non-node entity types.
- Not testable on MySQL — needs a real MariaDB 11.7+ instance.

## Issues

Bugs and feature requests should be reported in the [Issue Queue](https://github.com/backdrop-contrib/ai_search_provider_mariadb/issues).

## Credits

- Created for Backdrop CMS by [Justin Keiser](https://github.com/keiserjb).
- Developed with AI assistance.

## License

GNU General Public License, version 2 or later. See `LICENSE.txt`.
