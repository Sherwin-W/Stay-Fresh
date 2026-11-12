import { useState } from "react";
import { DashboardView } from "./components/DashboardView";
import { Login } from "./components/Login";
import { useAuth } from "./hooks/useAuth";
import { useDemoReadings } from "./hooks/useDemoReadings";
import { useReadings } from "./hooks/useReadings";
import { RANGES } from "./lib/derive";
import { DEVICE_ID, isConfigured } from "./lib/supabase";

const demoRequested = () => new URLSearchParams(window.location.search).get("demo") === "1";

function Live() {
  const auth = useAuth();
  const [rangeKey, setRangeKey] = useState("1h");
  const [demo, setDemo] = useState(demoRequested);
  const range = RANGES.find((r) => r.key === rangeKey)!;
  const signedIn = !!auth.session && !demo;
  const live = useReadings(DEVICE_ID, range.ms, signedIn);
  const demoData = useDemoReadings(range.ms);

  if (demo) {
    return <DashboardView deviceId="demo-fridge" state={demoData} rangeKey={rangeKey} onRange={setRangeKey} demo onExitDemo={() => setDemo(false)} />;
  }
  if (auth.loading) return <p className="center muted" role="status">Loading…</p>;
  if (!auth.session) return <Login onSignIn={auth.signIn} onDemo={() => setDemo(true)} />;
  return (
    <DashboardView deviceId={DEVICE_ID} state={live} rangeKey={rangeKey} onRange={setRangeKey} demo={false}
      userEmail={auth.session.user.email ?? undefined} onSignOut={auth.signOut} />
  );
}

export default function App() {
  const [demo, setDemo] = useState(demoRequested);
  const [rangeKey, setRangeKey] = useState("1h");
  const demoData = useDemoReadings((RANGES.find((r) => r.key === rangeKey) ?? RANGES[1]).ms);

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
