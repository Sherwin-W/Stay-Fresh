import { useMemo, useRef, useState } from "react";
import { useWidth } from "../hooks/useWidth";
import { splitSegments, type Point } from "../lib/derive";

interface Props {
  title: string;
  points: Point[]; // v === null means "no value" and breaks the line
  domain: [number, number]; // time window [start, end] in ms
  unit: string;
  digits?: number;
  minSpan?: number; // smallest y-range, so noise is not blown up
  refLine?: { v: number; label: string };
  height?: number;
  empty: string; // message when there is no data in the window
}

function niceTicks(lo: number, hi: number, n = 4): number[] {
  const span = hi - lo || 1;
  const raw = span / n;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
  const out: number[] = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(+v.toFixed(6));
  return out;
}

const timeFmt = (t: number, withSec: boolean) =>
  new Date(t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", ...(withSec ? { second: "2-digit" } : {}) });

/**
 * Single-series SVG line chart. Gaps (missing values, or >30 s without a
 * reading) are left as visible breaks: lines are never interpolated across
 * missing data. The x-axis always ends at "now", so a stale feed shows an
 * empty right edge instead of a frozen value pretending to be live.
 */
export function LineChart({ title, points, domain, unit, digits = 1, minSpan = 1, refLine, height = 220, empty }: Props) {
  const wrap = useRef<HTMLDivElement>(null);
  const width = useWidth(wrap);
  const [hover, setHover] = useState<Point | null>(null);

  const m = { l: 46, r: 14, t: 12, b: 26 };
  const iw = width - m.l - m.r;
  const ih = height - m.t - m.b;

  const valid = useMemo(() => points.filter((p): p is { t: number; v: number } => p.v !== null), [points]);
  const segments = useMemo(() => splitSegments(points), [points]);

  const [t0, t1] = domain;
  let lo = Infinity, hi = -Infinity;
  for (const p of valid) { lo = Math.min(lo, p.v); hi = Math.max(hi, p.v); }
  if (refLine) { lo = Math.min(lo, refLine.v); hi = Math.max(hi, refLine.v); }
  if (!isFinite(lo)) { lo = 0; hi = 1; }
  const pad = Math.max((hi - lo) * 0.12, 0);
  lo -= pad; hi += pad;
  if (hi - lo < minSpan) { const mid = (hi + lo) / 2; lo = mid - minSpan / 2; hi = mid + minSpan / 2; }
  const yTicks = niceTicks(lo, hi);
  const x = (t: number) => m.l + ((t - t0) / (t1 - t0)) * iw;
  const y = (v: number) => m.t + (1 - (v - lo) / (hi - lo)) * ih;
  const xTicks = Array.from({ length: 5 }, (_, i) => t0 + ((t1 - t0) * i) / 4);
  const withSec = t1 - t0 <= 20 * 60_000;

  const onMove = (e: React.PointerEvent<SVGRectElement>) => {
    if (!valid.length) return;
    const r = e.currentTarget.getBoundingClientRect();
    const t = t0 + ((e.clientX - r.left) / r.width) * (t1 - t0);
    let a = 0, b = valid.length - 1;
    while (b - a > 1) { const mid = (a + b) >> 1; if (valid[mid].t < t) a = mid; else b = mid; }
    setHover(Math.abs(valid[a].t - t) < Math.abs(valid[b].t - t) ? valid[a] : valid[b]);
  };

  const summary = valid.length
    ? `${title}: ${valid.length} readings, from ${valid[0].v.toFixed(digits)} to ${valid[valid.length - 1].v.toFixed(digits)} ${unit}, range ${Math.min(...valid.map((p) => p.v)).toFixed(digits)} to ${Math.max(...valid.map((p) => p.v)).toFixed(digits)}.`
    : `${title}: no data in this window.`;

  return (
    <div className="chart" ref={wrap}>
      {valid.length === 0 ? (
        <div className="chart-empty" style={{ height }}>{empty}</div>
      ) : (
        <>
          <svg width={width} height={height} role="img" aria-label={summary}>
            {yTicks.map((v) => (
              <g key={v}>
                <line className="gl" x1={m.l} x2={width - m.r} y1={y(v)} y2={y(v)} />
                <text className="tick" x={m.l - 8} y={y(v) + 4} textAnchor="end">{v.toFixed(Math.abs(v) < 10 ? 1 : 0)}</text>
              </g>
            ))}
            {xTicks.map((t, i) => (
              <text key={i} className="tick" x={Math.min(Math.max(x(t), m.l + 14), width - m.r - 14)} y={height - 6} textAnchor="middle">
                {timeFmt(t, withSec)}
              </text>
            ))}
            {refLine && (
              <g>
                <line className="ref" x1={m.l} x2={width - m.r} y1={y(refLine.v)} y2={y(refLine.v)} />
                <text className="tick ref-label" x={width - m.r} y={y(refLine.v) - 4} textAnchor="end">{refLine.label}</text>
              </g>
            )}
            {segments.map((seg, i) =>
              seg.length === 1 ? (
                <circle key={i} className="dot" cx={x(seg[0].t)} cy={y(seg[0].v!)} r={3} />
              ) : (
                <polyline key={i} className="line" fill="none"
                  points={seg.map((p) => `${x(p.t).toFixed(1)},${y(p.v!).toFixed(1)}`).join(" ")} />
              ),
            )}
            {hover && (
              <g pointerEvents="none">
                <line className="cross" x1={x(hover.t)} x2={x(hover.t)} y1={m.t} y2={m.t + ih} />
                <circle className="dot hover" cx={x(hover.t)} cy={y(hover.v!)} r={5} />
              </g>
            )}
            <rect x={m.l} y={m.t} width={iw} height={ih} fill="transparent"
              onPointerMove={onMove} onPointerLeave={() => setHover(null)} onPointerDown={onMove} />
          </svg>
          {hover && (
            <div className="tooltip" role="status"
              style={{ left: Math.min(Math.max(x(hover.t), 70), width - 70), top: Math.max(y(hover.v!) - 54, 0) }}>
              <strong>{hover.v!.toFixed(digits)} {unit}</strong>
              <span>{new Date(hover.t).toLocaleTimeString()}</span>
              <span className="fine">server arrival time</span>
            </div>
          )}
        </>
      )}
    </div>
  );
}
