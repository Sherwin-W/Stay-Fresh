import { useState } from "react";
import { DashboardView } from "./components/DashboardView";
import { useDemoReadings } from "./hooks/useDemoReadings";
import { useReadings } from "./hooks/useReadings";
import { ALERT_WINDOW_MS, RANGES } from "./lib/derive";
import { DEVICE_ID, isConfigured } from "./lib/supabase";

const demoRequested = () => new URLSearchParams(window.location.search).get("demo") === "1";

function Live() {
  const [rangeKey, setRangeKey] = useState("1h");
  const [demo, setDemo] = useState(demoRequested);
  const range = RANGES.find((r) => r.key === rangeKey)!;
  // Always hold at least ALERT_WINDOW_MS of history: the "above 4 °C for an hour" alert needs it
  // even when a short chart range is selected.
  const loadMs = Math.max(range.ms, ALERT_WINDOW_MS);
  const live = useReadings(DEVICE_ID, loadMs, !demo);
  const demoData = useDemoReadings(loadMs);

  if (demo) {
    return <DashboardView deviceId="demo-fridge" state={demoData} rangeKey={rangeKey} onRange={setRangeKey} demo onExitDemo={() => setDemo(false)} />;
  }
  return <DashboardView deviceId={DEVICE_ID} state={live} rangeKey={rangeKey} onRange={setRangeKey} demo={false} />;
}

export default function App() {
  const [demo, setDemo] = useState(demoRequested);
  const [rangeKey, setRangeKey] = useState("1h");
  const demoData = useDemoReadings(Math.max((RANGES.find((r) => r.key === rangeKey) ?? RANGES[1]).ms, ALERT_WINDOW_MS));

  if (!isConfigured) {
    if (demo) {
      return <DashboardView deviceId="demo-fridge" state={demoData} rangeKey={rangeKey} onRange={setRangeKey} demo onExitDemo={() => setDemo(false)} />;
    }
    return (
      <main className="login-wrap">
        <section className="glass login" data-testid="not-configured">
          <h1>Stay-Fresh</h1>
          <p>Supabase isn’t configured. Copy <code>.env.example</code> to <code>.env.local</code>, set <code>VITE_SUPABASE_URL</code> and <code>VITE_SUPABASE_PUBLISHABLE_KEY</code>, then restart <code>npm run dev</code>.</p>
          <button className="link" onClick={() => setDemo(true)}>Preview with demo data (not real readings)</button>
        </section>
      </main>
    );
  }
  return <Live />;
}
