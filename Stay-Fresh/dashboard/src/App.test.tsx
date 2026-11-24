import { render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { normalizeRow } from "./lib/derive";
import type { RawRow } from "./lib/types";

// --- mocks: no network, no real Supabase ---
vi.mock("./lib/supabase", () => ({
  isConfigured: true,
  DEVICE_ID: "fridge-05",
  getClient: () => ({}),
}));
const fetchReadings = vi.fn();
vi.mock("./lib/api", () => ({ fetchReadings: (...a: unknown[]) => fetchReadings(...a) }));

import App from "./App";

const row = (o: Partial<RawRow> = {}, ageSec = 3): ReturnType<typeof normalizeRow> =>
  normalizeRow({
    id: String(Math.random()), received_at: new Date(Date.now() - ageSec * 1000).toISOString(), device_id: "fridge-05",
    uptime_ms: 5000, temperature_c: 4.2, angle_deg: 0, door_open: false, alarm_active: false, open_duration_ms: 0, ...o,
  });

beforeEach(() => {
  fetchReadings.mockReset();
});

describe("no sign-in", () => {
  it("loads the dashboard straight away without any login form", async () => {
    fetchReadings.mockResolvedValue([]);
    render(<App />);
    await screen.findByTestId("freshness-pill");
    expect(screen.queryByRole("button", { name: "Sign in" })).not.toBeInTheDocument();
    expect(fetchReadings).toHaveBeenCalled();
  });
});

describe("dashboard", () => {
  it("shows an explicit empty state (not zeros)", async () => {
    fetchReadings.mockResolvedValue([]);
    render(<App />);
    expect(await screen.findByTestId("empty")).toHaveTextContent("No readings in this time range");
    expect(screen.getByTestId("no-data")).toBeInTheDocument();
    expect(screen.getByTestId("freshness-pill")).toHaveTextContent("No data");
  });

  it("shows live data with temperature and door", async () => {
    fetchReadings.mockResolvedValue([row({ temperature_c: 4.2 }, 3)]);
    render(<App />);
    expect(await screen.findByTestId("temp-value")).toHaveTextContent("4.2");
    expect(screen.getByTestId("freshness-pill")).toHaveTextContent("Live");
    expect(screen.queryByTestId("stale-banner")).not.toBeInTheDocument();
  });

  it("marks old data stale and does not call it live or blame Wi-Fi", async () => {
    fetchReadings.mockResolvedValue([row({ temperature_c: 4.2 }, 600)]);
    render(<App />);
    const banner = await screen.findByTestId("stale-banner");
    expect(banner).toHaveTextContent("last known");
    expect(banner).toHaveTextContent("does not say why");
    expect(screen.getByTestId("freshness-pill")).toHaveTextContent("Stale");
  });

  it("distinguishes a missing temperature from a real 0", async () => {
    fetchReadings.mockResolvedValue([row({ temperature_c: null })]);
    const { unmount } = render(<App />);
    expect(await screen.findByTestId("temp-missing")).toHaveTextContent("No reading");
    unmount();
    fetchReadings.mockResolvedValue([row({ temperature_c: 0 })]);
    render(<App />);
    expect(await screen.findByTestId("temp-value")).toHaveTextContent("0.0");
  });

  it("shows an unknown door as Unknown, never Closed", async () => {
    fetchReadings.mockResolvedValue([row({ door_state: "unknown", door_open: false, angle_quality: "none" })]);
    render(<App />);
    await screen.findByTestId("temp-value");
    expect(screen.getAllByText("Unknown").length).toBeGreaterThan(0);
    expect(screen.queryByText("Closed", { selector: ".big" })).not.toBeInTheDocument();
  });

  it("shows the alarm banner only for fresh data", async () => {
    fetchReadings.mockResolvedValue([row({ door_open: true, alarm_active: true, open_duration_ms: 12000 }, 2)]);
    render(<App />);
    expect(await screen.findByRole("alert")).toHaveTextContent("Alarm active");
  });

  it("keeps old data and shows a banner when a fetch fails", async () => {
    fetchReadings.mockRejectedValue(new Error("network down"));
    render(<App />);
    expect(await screen.findByTestId("fetch-error")).toHaveTextContent("network down");
  });
});

describe("demo mode", () => {
  it("is opt-in, clearly labelled, and never queries the database", async () => {
    window.history.pushState({}, "", "/?demo=1"); // demo is reached only via ?demo=1
    render(<App />);
    await waitFor(() => expect(screen.getByText("DEMO MODE")).toBeInTheDocument());
    expect(fetchReadings).not.toHaveBeenCalled();
  });
});
