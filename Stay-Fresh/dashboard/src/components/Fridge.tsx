import type { DoorState } from "../lib/types";

interface Props {
  angleDeg: number | null;
  state: DoorState | null;
  alarm: boolean;
  stale: boolean;
}

/**
 * CSS-3D refrigerator. The door rotates about its left hinge by the
 * ESTIMATED angle. It deliberately has no water/humidity imagery: this
 * project measures neither.
 */
export function Fridge({ angleDeg, state, alarm, stale }: Props) {
  let shown = angleDeg === null ? 0 : Math.min(Math.max(angleDeg, 0), 110);
  if (state === "open" && shown < 14) shown = 14; // keep "open" visibly open even at small estimates
  if (state === "closed" && shown > 8) shown = 8;
  if (state === "unknown" || state === null) shown = 0;

  const label =
    state === null ? "No door data"
    : state === "unknown" ? "Door state unknown"
    : `Door ${state}${angleDeg !== null ? `, about ${Math.round(angleDeg)} degrees (estimate)` : ""}`;

  return (
    <div className={`fridge-stage${alarm ? " alarm" : ""}${stale ? " stale" : ""}`} role="img" aria-label={`${label}${stale ? ", last known (data is stale)" : ""}`}>
      <div className="ripples" aria-hidden="true"><span /><span /><span /></div>
      <div className="fridge">
        <div className="fridge-body">
          <div className="fridge-interior" aria-hidden="true">
            <div className="shelf" /><div className="shelf" /><div className="shelf" />
          </div>
          <div className="fridge-door" style={{ transform: `rotateY(${-shown}deg)` }}>
            <div className="door-top" />
            <div className="door-handle" />
          </div>
        </div>
        <div className="fridge-feet" aria-hidden="true" />
      </div>
      {(state === null || state === "unknown") && <div className="fridge-badge">?</div>}
      {stale && <div className="fridge-note">Last known</div>}
    </div>
  );
}
