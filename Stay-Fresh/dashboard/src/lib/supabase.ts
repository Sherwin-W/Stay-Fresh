import { createClient, type SupabaseClient } from "@supabase/supabase-js";

export const SUPABASE_URL = (import.meta.env.VITE_SUPABASE_URL as string | undefined)?.trim();
export const SUPABASE_KEY = (import.meta.env.VITE_SUPABASE_PUBLISHABLE_KEY as string | undefined)?.trim();
export const DEVICE_ID = ((import.meta.env.VITE_DEVICE_ID as string | undefined) ?? "fridge-05").trim();

export const isConfigured = Boolean(SUPABASE_URL && SUPABASE_KEY);

let client: SupabaseClient | null = null;

/** Single shared client. Only the publishable key is ever used in the browser. */
export function getClient(): SupabaseClient {
  if (!isConfigured) throw new Error("Supabase is not configured");
  if (!client) {
    client = createClient(SUPABASE_URL!, SUPABASE_KEY!, {
      auth: { persistSession: true, autoRefreshToken: true },
    });
  }
  return client;
}
