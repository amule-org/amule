// REST client for the amuleapi /api/v0 surface.
//
// - Always sends the session cookie (credentials: "include").
// - Latches "session dead" on the first 401 and fails every later call
//   locally (see below) — the server bans an IP that keeps knocking.
// - Parses the {error:{code,message}} envelope into ApiError.
// - Caches ETags per GET target and revalidates with If-None-Match so a
//   304 short-circuits re-downloading/parsing an unchanged body.
// - Dynamic base path: derived from window.location.pathname so it works
//   both at the root (/) and under a reverse proxy subpath (e.g. /amule/).

import { t } from "./i18n.js";

const BASE = window.location.pathname.replace(/\/?$/, "/") + "api/v0";

export class ApiError extends Error {
  constructor(status, code, message) {
    super(message || code || ("HTTP " + status));
    this.name = "ApiError";
    this.status = status;
    this.code = code || "";
  }
}

// Failed entries of a bulk `results` envelope ([] if all ok / not a bulk resp).
export function bulkFailures(res) {
  return ((res && res.results) || []).filter((r) => !r.ok);
}

// path -> { etag, data }
const etagCache = new Map();

// Optional hook invoked when the session dies, so the shell can tear the
// live-data layer down and bounce to login. Called at most once per session
// with the reason ("unauthorized" | "rate_limited").
let onUnauthorized = null;
export function setUnauthorizedHandler(fn) { onUnauthorized = fn; }

// Session gate.
//
// amuleapi counts every 401 against the calling IP and locks the IP out for
// five minutes once it sees 30 within 60 s. Restarting amuleapi invalidates
// the session cookie, so without a latch the live-data layer walks straight
// into that ban: its fallback poll loop fires GET status plus one GET per
// active resource every 4 s, and the SSE stream reconnects on its own — every
// one of those a fresh 401.
//
// So the first rejection latches the client as logged out and every later
// call fails locally, with no fetch at all, until a successful login clears
// it. Only the auth endpoints bypass the gate (`noGate`), so signing back in
// still works.
let sessionDead = false;

function markSessionDead(reason) {
  if (sessionDead) return;
  sessionDead = true;
  if (onUnauthorized) onUnauthorized(reason);
}

async function request(method, path, { body, useEtag = false, noGate = false } = {}) {
  if (sessionDead && !noGate) {
    throw new ApiError(401, "unauthorized", "session ended");
  }

  const url = BASE + "/" + path.replace(/^\//, "");
  const headers = {};
  let cacheKey = null;

  if (useEtag && (method === "GET" || method === "HEAD")) {
    cacheKey = url;
    const cached = etagCache.get(cacheKey);
    // Verbatim: re-quoting a stripped `W/"abc"` gives `"W/abc"`, matching nothing.
    if (cached) headers["If-None-Match"] = cached.etag;
  }

  const init = { method, headers, credentials: "include" };
  if (body !== undefined) {
    headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(body);
  }

  let resp;
  try {
    resp = await fetch(url, init);
  } catch (networkErr) {
    throw new ApiError(0, "network", t("common_network_error", { message: networkErr.message }));
  }

  if (resp.status === 304 && cacheKey) {
    return etagCache.get(cacheKey).data;
  }

  // No-body success (e.g. 204).
  if (resp.status === 204) return {};

  const text = await resp.text();
  let payload = null;
  if (text) {
    try { payload = JSON.parse(text); } catch (_) { payload = null; }
  }

  if (!resp.ok) {
    const err = payload && payload.error ? payload.error : {};
    // 401 (cookie gone / expired / revoked) and the auth limiter's own 429
    // are terminal for this session. Match 429 on the code, not the status:
    // other endpoints answer 429 for their own throttles and must not log the
    // user out. POST /version/check is the one that bit -- it answered
    // `rate_limited` too, so "Check now" ended the session.
    if (!noGate && (resp.status === 401 || (resp.status === 429 && err.code === "rate_limited"))) {
      markSessionDead(resp.status === 429 ? "rate_limited" : "unauthorized");
    }
    throw new ApiError(resp.status, err.code, err.message);
  }

  if (cacheKey) {
    const etag = (resp.headers.get("ETag") || "").trim();
    if (etag) etagCache.set(cacheKey, { etag, data: payload });
  }

  return payload === null ? {} : payload;
}

// The API pages list responses at 100 rows by default. This UI does not page
// yet, so a list request asks for the whole collection explicitly rather than
// silently rendering the first 100 of N.
//
// 1e9 is the API's own ceiling: past any collection that can exist, and well
// inside JS's exact-integer range (2^53-1), so the number sent is the number
// that arrives. Do NOT reach for Number.MAX_SAFE_INTEGER here -- the server
// rejects anything above the ceiling, and a value that large would round on
// the way through a float anyway.
export const LIMIT_ALL = 1000000000;

export const api = {
  get: (path, opts) => request("GET", path, { useEtag: true, ...opts }),

  // A list GET. Distinct from get() so the "give me all of it" decision lives
  // in one place: when this UI grows real pagination, the call sites stay and
  // only this changes.
  list: (path, opts) =>
    request("GET", path + (path.includes("?") ? "&" : "?") + "limit=" + LIMIT_ALL,
            { useEtag: true, ...opts }),
  post: (path, body) => request("POST", path, { body }),
  patch: (path, body) => request("PATCH", path, { body }),
  del: (path, body) => request("DELETE", path, { body }),

  // auth. login/session bypass the gate (they are how a dead session comes
  // back); a winning login re-arms the client. logout stays gated on purpose:
  // once the session is dead the server would only answer it with another
  // 401, and the caller treats a failure as "logged out" anyway.
  login: async (password) => {
    const res = await request("POST", "auth/login", { body: { password }, noGate: true });
    sessionDead = false;
    etagCache.clear(); // fresh backend, possibly a fresh dataset
    return res;
  },
  logout: () => request("POST", "auth/logout"),
  session: () => request("GET", "auth/session", { noGate: true }),
};
