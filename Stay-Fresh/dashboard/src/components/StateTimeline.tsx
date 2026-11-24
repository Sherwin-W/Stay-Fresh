import { useRef } from "react";
import { useWidth } from "../hooks/useWidth";
import { buildSpans } from "../lib/derive";
import type { Reading } from "../lib/types";

interface Props {
  rows: Reading[];
  domain: [number, number];
}

const t = (ms: number) => new Date(ms).toLocaleTimeString();

/** Two lanes: door state and alarm. Gaps are drawn as gaps ("no data"). */
export function StateTimeline({ rows, domain }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const w = useWidth(ref);

  const [t0, t1] = domain;
  const l = 46, r = 14, iw = w - l - r;
  const x = (ms: number) => l + (Math.min(Math.max(ms, t0), t1) - t0) / (t1 - t0) * iw;
  const spans = buildSpans(rows).filter((s) => s.end > t0 && s.start < t1);
  const lane = (y: number, label: string) => (
    <>
      <text className="tick" x={l - 8} y={y + 17} textAnchor="end">{label}</text>
      <rect className="lane-bg" x={l} y={y} width={iw} height={24} rx={4} />
    </>
  );

  return (
    <div className="chart" ref={ref}>
      <svg width={w} height={86} role="img"
        aria-label={`Door and alarm timeline. ${spans.filter((s) => s.kind === "open").length} open period(s) and ${spans.filter((s) => s.alarm).length} alarm period(s) recorded in this window.`}>
        {lane(6, "Door")}
        {lane(42, "Alarm")}
        {spans.map((s, i) => {
          const w2 = Math.max(x(s.end) - x(s.start), 1.5);
          const label = `${s.kind === "nodata" ? "No data" : s.kind} · ${t(s.start)} – ${t(s.end)}`;
          return (
            <g key={i}>
              {s.kind !== "nodata" && (
                <rect className={`span ${s.kind}`} x={x(s.start)} y={6} width={w2} height={24} rx={2}>
                  <title>{label}</title>
                </rect>
              )}
              {s.alarm && (
                <rect className="span alarm" x={x(s.start)} y={42} width={w2} height={24} rx={2}>
                  <title>{`Alarm active · ${t(s.start)} – ${t(s.end)}`}</title>
                </rect>
              )}
            </g>
          );
        })}
        <text className="tick" x={l} y={82}>{new Date(t0).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}</text>
        <text className="tick" x={w - r} y={82} textAnchor="end">now</text>
      </svg>
      <ul className="legend" aria-label="Timeline legend">
        <li><i className="sw closed" /> Closed</li>
        <li><i className="sw open" /> Open</li>
        <li><i className="sw unknown" /> Unknown</li>
        <li><i className="sw alarm" /> Alarm active</li>
        <li><i className="sw nodata" /> No data</li>
      </ul>
    </div>
  );
}
