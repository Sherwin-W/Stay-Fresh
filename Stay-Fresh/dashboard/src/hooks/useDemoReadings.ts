import { useMemo } from "react";
import { makeDemoReadings } from "../lib/demo";
import type { ReadingsState } from "./useReadings";
import { useNow } from "./useNow";

/** Synthetic feed for the opt-in demo mode. Never calls Supabase. */
export function useDemoReadings(rangeMs: number): ReadingsState {
  const tick = Math.floor(useNow(5000) / 5000);
  return useMemo(
    () => ({ rows: makeDemoReadings(tick * 5000, rangeMs), status: "ready" as const, error: null, lastOkAt: tick * 5000, truncated: false }),
    [tick, rangeMs],
  );
}
