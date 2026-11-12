import { useEffect, useRef, useState } from "react";
import { fetchReadings } from "../lib/api";
import { MAX_ROWS, PAGE_SIZE, mergeReadings } from "../lib/derive";
import { getClient } from "../lib/supabase";
import type { Reading } from "../lib/types";

export const POLL_MS = 5000;

export interface ReadingsState {
  rows: Reading[]; // ascending by received_at
  status: "loading" | "ready" | "error";
  error: string | null; // last error, rows from earlier fetches are kept
  lastOkAt: number | null; // browser time of last successful fetch
  truncated: boolean; // true if the window holds more rows than we loaded
}

/**
 * Initial load pages backwards from "now" (so the newest data always arrives
 * first, up to MAX_ROWS). After that, one request every POLL_MS fetches only
 * rows newer than what we have. Requests never overlap: the next poll is
 * scheduled only after the previous one finished. Cleanup stops the loop.
 */
export function useReadings(deviceId: string, rangeMs: number, enabled: boolean): ReadingsState {
  const [state, setState] = useState<ReadingsState>({
    rows: [], status: "loading", error: null, lastOkAt: null, truncated: false,
  });
  const rowsRef = useRef<Reading[]>([]);

  useEffect(() => {
    if (!enabled) return;
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const client = getClient();
    rowsRef.current = [];
    setState({ rows: [], status: "loading", error: null, lastOkAt: null, truncated: false });

    const windowStart = () => Date.now() - rangeMs;
    const fail = (e: unknown) =>
      setState((s) => ({ ...s, status: "error", error: e instanceof Error ? e.message : String(e) }));

    async function initialLoad(): Promise<void> {
      let before: string | undefined;
      let truncated = false;
      let loaded = 0;
      const sinceIso = new Date(windowStart()).toISOString();
      for (;;) {
        const page = await fetchReadings(client, { deviceId, sinceIso, beforeIso: before, ascending: false });
        if (cancelled) return;
        rowsRef.current = mergeReadings(rowsRef.current, page, windowStart());
        loaded += page.length;
        if (page.length < PAGE_SIZE) break;
        if (loaded >= MAX_ROWS) { truncated = true; break; }
        before = new Date(Math.min(...page.map((r) => r.receivedAt))).toISOString();
        setState((s) => ({ ...s, rows: rowsRef.current, status: "ready", truncated: false }));
      }
      setState({ rows: rowsRef.current, status: "ready", error: null, lastOkAt: Date.now(), truncated });
    }

    async function poll(): Promise<void> {
      const latest = rowsRef.current[rowsRef.current.length - 1];
      const sinceIso = new Date(windowStart()).toISOString();
      const fresh = await fetchReadings(client, {
        deviceId, sinceIso, afterIso: latest ? new Date(latest.receivedAt).toISOString() : undefined, ascending: true,
      });
      if (cancelled) return;
      rowsRef.current = mergeReadings(rowsRef.current, fresh, windowStart());
      setState((s) => ({ ...s, rows: rowsRef.current, status: "ready", error: null, lastOkAt: Date.now() }));
    }

    let initialDone = false;
    async function loop() {
      try {
        if (!initialDone) {
          await initialLoad(); // retried on the next tick if it throws
          initialDone = true;
        } else if (!document.hidden) {
          await poll(); // skipped while the tab is hidden
        }
      } catch (e) {
        if (!cancelled) fail(e);
      }
      if (!cancelled) timer = setTimeout(loop, POLL_MS);
    }
    loop();

    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
    };
  }, [deviceId, rangeMs, enabled]);

  return state;
}
