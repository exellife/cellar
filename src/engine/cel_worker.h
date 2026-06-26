/* ============================================================================
 * cellar — background job worker.
 *
 * A single process-wide thread that, every `interval_secs`, runs each open app's
 * due background jobs (claimed from its JobQueue, dispatched to its `job` hook).
 * This is the autonomous driver behind POST /jobs/run — so listings expire,
 * alerts fire, and rollups run without an external trigger.
 *
 * interval_secs <= 0 disables it (e.g. tests that drive /jobs/run deterministically).
 * ============================================================================ */
#ifndef CEL_WORKER_H
#define CEL_WORKER_H

void cel_worker_start(int interval_secs);
void cel_worker_stop(void);   /* signals + joins the thread (idempotent) */

#endif /* CEL_WORKER_H */
