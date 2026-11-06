# Fathom

Syzygy tablebase probing code from [Fathom](https://github.com/jdart1/Fathom)
(commit c9c6fef0dddc05d2e242c183acf5833149ab676d), MIT licensed (see
`LICENSE`). It is based on Ronald de Man's original probing code, as is
Stockfish's.

Changes for AetherZ, all in `tbconfig.h`:

- `TB_VALUE_MATE` 30000 and `TB_MAX_MATE_PLY` 128, AetherZ's `mate_score`
  and `max_search_ply`, so that root move scores are in the engine's units
  (a tablebase win is `mate_score - max_search_ply - 1`).
- `TB_NO_HELPER_API`: the engine does not use the helper functions.
