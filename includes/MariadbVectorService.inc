<?php
/**
 * @file
 * MariaDB native VECTOR Search API backend for Backdrop CMS.
 *
 * Ported from the pgvector provider. MariaDB is Backdrop's native engine, so
 * no custom database driver is required — db_query() talks to it directly.
 * Requires MariaDB >= 11.7 (11.8 LTS recommended) for the VECTOR type,
 * VEC_FromText()/VEC_DISTANCE_*() functions, and VECTOR INDEX (HNSW).
 *
 * Note: with MariaDB the distance metric is fixed at table-creation time via
 * the VECTOR INDEX DISTANCE= option. Changing metric_type after a table exists
 * has no effect on the index; rebuild the table to change it.
 */

require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendPluginBase.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackendInterface.inc';
require_once backdrop_get_path('module', 'ai_search') . '/includes/AiSearchBackend.inc';

class MariadbVectorService extends SearchApiAbstractService implements AiSearchBackendInterface {

  const TABLE_PREFIX = 'search_vector_';

  protected $configuration;

  public function __construct($configuration = []) {
    parent::__construct($configuration);
    if (is_object($configuration) && property_exists($configuration, 'options')) {
      $this->configuration = (array) $configuration->options;
    }
    elseif (is_array($configuration)) {
      $this->configuration = $configuration;
    }
    else {
      $this->configuration = [];
    }
  }

  public function defaultConfiguration() {
    return parent::defaultConfiguration() + [
      // Uses the site's default database connection.
      'table'       => '', // default to computed per-index table name
      'metric_type' => 'COSINE', // COSINE | L2 (euclidean)
      'hnsw_m'      => 6,        // HNSW M parameter (connections per node)
    ];
  }

  public function configurationForm($form, &$form_state) {
    $embedding_configuration = ai_search_default_engine_configuration();
    if (!empty($this->options['embeddings_engine'])) {
      $embedding_configuration['embeddings_engine'] = $this->options['embeddings_engine'];
    }
    if (!empty($this->options['embeddings_engine_configuration'])) {
      $embedding_configuration['embeddings_engine_configuration'] = $this->options['embeddings_engine_configuration'];
    }
    $form = ai_search_engine_configuration_form($form, $form_state, $embedding_configuration);

    $form['table'] = [
      '#type' => 'textfield',
      '#title' => t('Table name (optional)'),
      '#description' => t('Leave empty to auto-generate per index, prefixed with @p.', ['@p' => self::TABLE_PREFIX]),
      '#default_value' => !empty($this->options['table']) ? $this->options['table'] : '',
      '#pattern' => '[a-zA-Z0-9_]*',
    ];
    $form['metric_type'] = [
      '#type' => 'select',
      '#title' => t('Metric Type'),
      '#description' => t('Distance metric for nearest-neighbor search. Fixed at table creation in MariaDB.'),
      '#default_value' => !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE',
      '#required' => TRUE,
      '#options' => [
        'L2' => 'L2 (euclidean)',
        'COSINE' => 'COSINE',
      ],
    ];
    $form['hnsw_m'] = [
      '#type' => 'number',
      '#title' => t('HNSW M'),
      '#description' => t('Number of connections per node in the HNSW graph. Higher = better recall, more storage. Default 6.'),
      '#default_value' => !empty($this->options['hnsw_m']) ? (int) $this->options['hnsw_m'] : 6,
      '#min' => 3,
    ];

    $form['help'] = [
      '#markup' => '<p>' . t('This backend uses the site database. It requires MariaDB 11.7+ (11.8 LTS recommended) with native VECTOR support. No extension or driver is needed.') . '</p>',
    ];

    $form = ai_search_reranking_configuration_form($form, $form_state, $this->options);
    return $form;
  }

  /**
   * Index items into the MariaDB vector table.
   */
  public function indexItems(SearchApiIndex $index, array $items) {
    global $ai_search_is_indexing;
    $ai_search_is_indexing = TRUE;

    if (empty($items)) {
      return [];
    }

    $dimension = isset($this->options['embeddings_engine_configuration']['dimension'])
      ? (int) $this->options['embeddings_engine_configuration']['dimension']
      : ai_search_get_embedding_dimension();

    $table = $this->getTableName($index);
    $this->ensureTable($table, $dimension);

    $expected_dimension = $dimension;

    $successful_ids = [];

    // Build a map of entity_type => [ids] from $items.
    //
    // Search API keys $items by the item ID. For an entity index this is the
    // raw entity ID (e.g. "192"), and the entity type comes from the index
    // itself. Some configurations instead use a canonical/compound key like
    // "entity:node/192:en" or "node/192"; handle those too. The item VALUE is
    // a field array and does not carry ->nid/->tid/->item_id, so those object
    // properties are only a last-resort fallback.
    $index_entity_type = method_exists($index, 'getEntityType') ? $index->getEntityType() : (!empty($index->item_type) ? $index->item_type : NULL);
    $ids_by_type = [];
    foreach ($items as $id => $item) {
      $entity_type = NULL;
      $entity_id = NULL;

      if (is_string($id) && strpos($id, '/') !== FALSE) {
        // Compound key: "entity:node/192:en" or "node/192".
        $raw = (strncmp($id, 'entity:', 7) === 0) ? substr($id, 7) : $id;
        list($entity_type, $id_part) = explode('/', $raw, 2);
        // Strip any language/revision suffix (e.g. "192:en" => "192").
        $entity_id = (int) (strpos($id_part, ':') !== FALSE ? strstr($id_part, ':', TRUE) : $id_part);
      }
      elseif (is_numeric($id) && $index_entity_type) {
        // Plain entity-ID key; entity type is the index's item type.
        $entity_type = $index_entity_type;
        $entity_id = (int) $id;
      }
      else {
        if (is_array($item)) $item = (object) $item;
        if (isset($item->nid)) {
          $entity_type = 'node';
          $entity_id = is_array($item->nid) && isset($item->nid['value']) ? (int) $item->nid['value'] : (int) $item->nid;
        }
        elseif (isset($item->tid)) {
          $entity_type = 'taxonomy_term';
          $entity_id = is_array($item->tid) && isset($item->tid['value']) ? (int) $item->tid['value'] : (int) $item->tid;
        }
        elseif (isset($item->item_id)) {
          $entity_type = 'paragraphs_item';
          $entity_id = is_array($item->item_id) && isset($item->item_id['value']) ? (int) $item->item_id['value'] : (int) $item->item_id;
        }
      }

      if ($entity_type && $entity_id) {
        $ids_by_type[$entity_type][] = $entity_id;
      }
    }

    $qualified = $this->qualify($table);

    foreach ($ids_by_type as $entity_type => $ids) {
      foreach (array_unique($ids) as $entity_id) {
        $rows = ai_search_normalize_vectors($entity_type, $entity_id, $expected_dimension, $index->machine_name);
        if (empty($rows)) {
          continue;
        }
        foreach ($rows as $r) {
          // Compose stable long id.
          $long_id = "entity:{$entity_type}/{$entity_id}:en:{$r['field_name']}:{$r['chunk_index']}";
          $backdrop_entity_id = "entity:{$entity_type}/{$entity_id}:en";
          $nid = ($entity_type === 'node') ? (int) $entity_id : NULL;

          $values = array_map('floatval', $r['values']);
          $vector_literal = '[' . implode(',', $values) . ']';

          // Mirror AIMilvusService::indexItems(): keep all contextual fields
          // from $r['metadata'] (title, url, taxonomy, etc.), not just content.
          $metadata = array_merge(
            $r['metadata'] ?? [],
            [
              'content'         => $r['metadata']['content'] ?? '',
              'title'           => $r['metadata']['title'] ?? NULL,
              'search_api_url'  => $r['metadata']['search_api_url'] ?? NULL,
              'url'             => $r['metadata']['url'] ?? NULL,
            ]
          );
          $metadata_json = json_encode(array_filter($metadata, function ($v) { return $v !== NULL; }));

          // MariaDB upsert: VEC_FromText() parses the JSON-array text into a
          // binary VECTOR; ON DUPLICATE KEY UPDATE handles re-indexing.
          // `vector` is a reserved word in MariaDB 11.7+, so the column must be
      // backtick-quoted in every statement that references it.
      $sql = "INSERT INTO {$qualified} (id, backdrop_entity_id, nid, content, metadata, `vector`) "
               . "VALUES (:id, :beid, :nid, :content, :metadata, VEC_FromText(:vector)) "
               . "ON DUPLICATE KEY UPDATE "
               . "backdrop_entity_id = VALUES(backdrop_entity_id), "
               . "nid = VALUES(nid), content = VALUES(content), metadata = VALUES(metadata), `vector` = VALUES(`vector`)";

          db_query($sql, [
            ':id' => $long_id,
            ':beid' => $backdrop_entity_id,
            ':nid' => $nid,
            ':content' => isset($r['metadata']['content']) ? (string) $r['metadata']['content'] : '',
            ':metadata' => $metadata_json,
            ':vector' => $vector_literal,
          ]);
          $successful_ids[] = $entity_id;
        }
      }
    }

    return array_unique($successful_ids);
  }

  /**
   * Delete items from the MariaDB vector table.
   */
  public function deleteItems($ids = 'all', ?SearchApiIndex $index = NULL) {
    $table = $this->getTableName($index);
    $qualified = $this->qualify($table);

    if ($ids === 'all') {
      db_query("TRUNCATE TABLE {$qualified}");
      return;
    }

    $ids = is_array($ids) ? array_filter($ids) : [];
    if (empty($ids)) {
      return;
    }

    // Build list of backdrop_entity_id values to delete.
    $beids = [];
    foreach ($ids as $id) {
      $id = (int) $id;
      $beids[] = "entity:node/{$id}:en"; // current indices generally use nodes; extend as needed.
    }
    $placeholders = [];
    $args = [];
    foreach ($beids as $i => $beid) {
      $ph = ":b{$i}";
      $placeholders[] = $ph;
      $args[$ph] = $beid;
    }
    $sql = "DELETE FROM {$qualified} WHERE backdrop_entity_id IN (" . implode(',', $placeholders) . ")";
    db_query($sql, $args);
  }

  /**
   * Perform a nearest-neighbor search using MariaDB vector distance.
   */
  public function search(SearchApiQueryInterface $query) {
    $index = $query->getIndex();
    $index_options = isset($index->options) ? $index->options : [];

    // Model selection: server options (canonical) → service embedding_model fallback.
    $model = $this->options['embeddings_engine'] ?? $this->options['embedding_model'] ?? NULL;

    if (empty($model)) {
      watchdog('ai_search_mariadb', 'No model configured!', [], WATCHDOG_ERROR);
      return ['result count' => 0, 'results' => []];
    }

    // Extract search text from keys.
    $keys = $query->getKeys();
    $text = is_array($keys)
      ? implode(' ', array_filter($keys, function ($key) { return is_string($key) && $key !== 'AND' && $key !== '#conjunction'; }))
      : (string) $keys;
    $text = trim($text);
    if ($text === '') {
      return ['result count' => 0, 'results' => []];
    }

    // Resolve client and request embedding using compatibility helpers.
    $api = NULL; $stripped_model = NULL; $model_to_use = NULL;
    $api = ai_get_api_for_model($model, $stripped_model);
    $model_to_use = !empty($stripped_model) ? $stripped_model : $model;

    if ($api && method_exists($api, 'recordLog')) {
      $api->recordLog('search_embedding', $model, ['text_len' => strlen($text)], NULL, TRUE, 0);
    }

    if (!$api) {
      watchdog('ai_search_mariadb', 'Failed to resolve API client for model: @model', ['@model' => $model], WATCHDOG_ERROR);
      throw new Exception('Failed to get API client for model: ' . $model);
    }

    $query_embedding = $api->embedding(ai_search_apply_embedding_task_prefix($text, $model_to_use, 'query'), $model_to_use);
    if (empty($query_embedding)) {
      throw new Exception('Failed to retrieve embedding.');
    }

    $top_k = (int) ($query->getOption('top_k') ?? 8);
    return $this->queryVector($index, $query_embedding, $top_k, $text, $query->getIndex()->getEntityType());
  }

  /**
   * Run a similarity search against an already-computed embedding vector.
   *
   * Factored out of search() so RAG callers (e.g. ai_search's generic
   * backend dispatch used by ai_agents/ai_assistants tools) can query with a
   * vector they already generated, without needing a SearchApiQueryInterface.
   *
   * @param SearchApiIndex $index
   *   The index to query.
   * @param array $vector
   *   The precomputed query embedding.
   * @param int $top_k
   *   Number of results to return.
   * @param string $rerank_text
   *   Original query text, used for metadata reranking (optional).
   * @param string|null $entity_type_for_ids
   *   Entity type to compare against for item-id shortening; defaults to the
   *   index's own entity type.
   *
   * @return array
   *   ['result count' => int, 'results' => array].
   */
  public function queryVector(SearchApiIndex $index, array $vector, $top_k = 8, $rerank_text = '', $entity_type_for_ids = NULL) {
    $top_k = (int) $top_k;
    $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
    $distance_fn = $this->metricFunction($metric);

    $table = $this->getTableName($index);
    $qualified = $this->qualify($table);

    $vector_literal = '[' . implode(',', array_map('floatval', $vector)) . ']';

    // ORDER BY the distance function drives use of the VECTOR INDEX. top_k is
    // cast to int and inlined (LIMIT placeholder binding is unreliable).
    $sql = "SELECT *, {$distance_fn}(`vector`, VEC_FromText(:q)) AS distance "
         . "FROM {$qualified} "
         . "ORDER BY {$distance_fn}(`vector`, VEC_FromText(:q)) "
         . "LIMIT " . $top_k;
    $result = db_query($sql, [
      ':q' => $vector_literal,
    ]);

    // Collect DB rows and normalize for reranking.
    $normalized_matches = [];
    foreach ($result as $row) {
      $row_array = (array) $row;
      $metadata = array_diff_key($row_array, array_flip(['vector', 'distance', 'metadata']));
      // Stored contextual fields (title, url, taxonomy, etc.) take precedence
      // over the raw table columns they were derived from.
      if (!empty($row->metadata)) {
        $decoded = json_decode($row->metadata, TRUE);
        if (is_array($decoded)) {
          $metadata = array_merge($metadata, $decoded);
        }
      }
      $normalized_matches[] = [
        'score'    => $this->distanceToSimilarity((float) $row->distance, $metric),
        'metadata' => $metadata,
        '_raw'     => $row,
      ];
    }

    $rerank_settings = !empty($this->options['metadata_reranking']) ? $this->options['metadata_reranking'] : [];
    ai_search_apply_metadata_reranking($normalized_matches, $rerank_text, $rerank_settings);

    $entity_type_for_ids = $entity_type_for_ids ?: $index->getEntityType();
    $results = [];
    foreach ($normalized_matches as $nm) {
      $row = $nm['_raw'];
      $entity_type = NULL; $entity_id = NULL;
      if (!empty($row->backdrop_entity_id) && preg_match('/^entity:([^\/]+)\/(\d+)/', $row->backdrop_entity_id, $m)) {
        $entity_type = $m[1];
        $entity_id = $m[2];
      }
      elseif (!empty($row->nid)) {
        $entity_type = 'node';
        $entity_id = $row->nid;
      }
      if ($entity_type && $entity_id) {
        $item_id = $entity_type == $entity_type_for_ids ? $entity_id : "{$entity_type}/{$entity_id}";
        $results[$item_id] = [
          'id'      => $item_id,
          'score'   => $nm['score'],
          'fields'  => [],
          'entity'  => entity_load($entity_type, $entity_id),
          'excerpt' => isset($row->content) ? AIContentExtractor::htmlToMarkdown($row->content) : '',
          'metadata' => $nm['metadata'],
        ];
      }
    }

    return [
      'result count' => count($results),
      'results' => $results,
    ];
  }

  public function supportsDataType($type) {
    return in_array($type, ['embeddings']);
  }

  // Embedding engine helpers required by interface.
  public function loadEmbeddingsEngine() {
    return ai_search_load_embeddings_engine($this->configuration);
  }
  public function getEmbeddingsEngine() {
    return isset($this->configuration['embeddings_engine']) ? $this->configuration['embeddings_engine'] : '';
  }
  public function getEmbeddingEnginesOptions() {
    return ai_search_get_embedding_engines_options();
  }
  public function updateEmbeddingConfigurationForm($form, &$form_state) {
    return ai_search_update_embedding_configuration_form($form, $form_state);
  }

  // Internal helpers.
  protected function getTableName($index) {
    $base = !empty($this->options['table']) ? $this->options['table'] : self::TABLE_PREFIX . $index->machine_name;
    return preg_replace('/[^a-zA-Z0-9_]/', '_', $base);
  }
  protected function qualify($table) {
    $table = preg_replace('/[^a-zA-Z0-9_]/', '', $table);
    return '`' . $table . '`';
  }

  /**
   * Ensure the vector table and index exist with the correct dimension.
   */
  protected function ensureTable($table, $dimension) {
    $qualified = $this->qualify($table);

    // Does the table exist?
    $exists = FALSE;
    try {
      db_query('SELECT 1 FROM ' . $qualified . ' LIMIT 1');
      $exists = TRUE;
    }
    catch (\Exception $e) {
      $exists = FALSE;
    }

    if (!$exists) {
      $metric = !empty($this->options['metric_type']) ? $this->options['metric_type'] : 'COSINE';
      $distance = $this->metricDistanceOption($metric);
      $m = !empty($this->options['hnsw_m']) ? (int) $this->options['hnsw_m'] : 6;

      // The indexed VECTOR column must be NOT NULL in MariaDB. The DISTANCE=
      // option fixes the metric for the HNSW index at creation time.
      // `vector` is a reserved word in MariaDB 11.7+, so the column and its
      // index reference must be backtick-quoted.
      // The primary key must be <= 256 bytes for a table carrying a VECTOR
      // INDEX. utf8mb4 would make VARCHAR(200) 800 bytes, so pin the id column
      // to single-byte ascii (ids are ascii: "entity:node/<id>:en:<field>:<n>").
      $sql = 'CREATE TABLE ' . $qualified . ' (
        id VARCHAR(200) CHARACTER SET ascii NOT NULL PRIMARY KEY,
        backdrop_entity_id VARCHAR(200),
        nid INT NULL,
        content LONGTEXT,
        metadata LONGTEXT,
        `vector` VECTOR(' . (int) $dimension . ') NOT NULL,
        VECTOR INDEX (`vector`) M=' . $m . ' DISTANCE=' . $distance . '
      ) ENGINE=InnoDB';
      db_query($sql);
    }
    else {
      // Validate dimension; if wrong, warn (dropping data would be risky).
      try {
        $column_type = db_query(
          "SELECT COLUMN_TYPE FROM information_schema.COLUMNS
           WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = :t AND COLUMN_NAME = 'vector'",
          [':t' => preg_replace('/[^a-zA-Z0-9_]/', '', $table)]
        )->fetchField();
        $dim = NULL;
        if ($column_type && preg_match('/vector\((\d+)\)/i', $column_type, $cm)) {
          $dim = (int) $cm[1];
        }
      }
      catch (\Exception $e) {
        $dim = NULL;
      }
      if ($dim && (int) $dim !== (int) $dimension) {
        watchdog('ai_search_mariadb', 'Existing table @t vector dimension (@d1) differs from configured (@d2).', ['@t' => $qualified, '@d1' => $dim, '@d2' => $dimension], WATCHDOG_WARNING);
      }

      // Backfill the metadata column on tables created before it existed.
      $has_metadata_col = db_query(
        "SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = :t AND COLUMN_NAME = 'metadata'",
        [':t' => preg_replace('/[^a-zA-Z0-9_]/', '', $table)]
      )->fetchField();
      if (!$has_metadata_col) {
        db_query('ALTER TABLE ' . $qualified . ' ADD COLUMN metadata LONGTEXT');
      }
    }
  }

  /**
   * Distance function name for the SELECT (must match the index metric).
   */
  protected function metricFunction($metric) {
    switch (strtoupper($metric)) {
      case 'L2':
        return 'VEC_DISTANCE_EUCLIDEAN';
      case 'COSINE':
      default:
        return 'VEC_DISTANCE_COSINE';
    }
  }

  /**
   * Convert a raw distance value into a "higher is better" similarity score.
   *
   * VEC_DISTANCE_* functions return distance (0 = identical), but every
   * caller of queryVector()/search() -- including ai_search_search_block's
   * score_threshold filter -- assumes score means similarity (higher is a
   * better match), matching how other backends like milvus already report
   * it. Returning raw distance silently inverts every threshold/ranking
   * decision downstream: the best matches get rejected as "too low" and
   * weak matches pass as "good enough".
   */
  protected function distanceToSimilarity($distance, $metric) {
    if (strtoupper($metric) === 'L2') {
      // Euclidean distance is unbounded; map it monotonically into (0, 1].
      return 1 / (1 + $distance);
    }
    // Cosine distance (VEC_DISTANCE_COSINE) ranges 0 (identical) to 2
    // (opposite); cosine similarity is the standard 1 - distance.
    return 1 - $distance;
  }

  /**
   * DISTANCE= option value for VECTOR INDEX creation.
   */
  protected function metricDistanceOption($metric) {
    switch (strtoupper($metric)) {
      case 'L2':
        return 'euclidean';
      case 'COSINE':
      default:
        return 'cosine';
    }
  }
}
