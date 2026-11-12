import { useState } from "react";

interface Props {
  onSignIn: (email: string, password: string) => Promise<string | null>;
  onDemo: () => void;
}

export function Login({ onSignIn, onDemo }: Props) {
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (e: React.FormEvent) => {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      setError(await onSignIn(email.trim(), password));
    } catch (err) {
      setError(err instanceof Error ? err.message : "Sign-in failed");
    } finally {
      setBusy(false);
    }
  };

  return (
    <main className="login-wrap">
      <form className="glass login" onSubmit={submit} aria-labelledby="login-title">
        <h1 id="login-title">Stay-Fresh</h1>
        <p className="muted">Sign in to view the fridge-door monitor. Readings are private: the database only returns rows owned by the account you sign in with.</p>
        <label>Email
          <input type="email" autoComplete="username" required value={email} onChange={(e) => setEmail(e.target.value)} />
        </label>
        <label>Password
          <input type="password" autoComplete="current-password" required value={password} onChange={(e) => setPassword(e.target.value)} />
        </label>
        {error && <p className="error" role="alert">Could not sign in: {error}</p>}
        <button className="primary" type="submit" disabled={busy}>{busy ? "Signing in…" : "Sign in"}</button>
        <button className="link" type="button" onClick={onDemo}>Preview with demo data (not real readings)</button>
      </form>
    </main>
  );
}
