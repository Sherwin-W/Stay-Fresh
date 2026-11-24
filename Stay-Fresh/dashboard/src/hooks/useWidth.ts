import { useEffect, useLayoutEffect, useState, type RefObject } from "react";

/** Width of an element: measured immediately on mount, then kept in sync. */
export function useWidth(ref: RefObject<HTMLElement>, fallback = 640): number {
  const [w, setW] = useState(fallback);
  useLayoutEffect(() => {
    const first = ref.current?.getBoundingClientRect().width;
    if (first) setW(Math.max(280, Math.floor(first)));
  }, [ref]);
  useEffect(() => {
    const el = ref.current;
    if (!el || typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(([e]) => setW(Math.max(280, Math.floor(e.contentRect.width))));
    ro.observe(el);
    return () => ro.disconnect();
  }, [ref]);
  return w;
}
