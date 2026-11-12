import { useCallback, useEffect, useState } from "react";
import type { Session } from "@supabase/supabase-js";
import { getClient } from "../lib/supabase";

export interface AuthState {
  session: Session | null;
  loading: boolean;
  signIn: (email: string, password: string) => Promise<string | null>; // error message or null
  signOut: () => Promise<void>;
}

export function useAuth(): AuthState {
  const [session, setSession] = useState<Session | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const client = getClient();
    let active = true;
    client.auth.getSession().then(({ data }) => {
      if (!active) return;
      setSession(data.session);
      setLoading(false);
    }).catch(() => active && setLoading(false));
    const { data } = client.auth.onAuthStateChange((_evt, s) => setSession(s));
    return () => {
      active = false;
      data.subscription.unsubscribe(); // clean up the listener
    };
  }, []);

  const signIn = useCallback(async (email: string, password: string) => {
    const { error } = await getClient().auth.signInWithPassword({ email, password });
    return error ? error.message : null;
  }, []);

  const signOut = useCallback(async () => {
    await getClient().auth.signOut();
  }, []);

  return { session, loading, signIn, signOut };
}
