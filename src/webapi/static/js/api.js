// REST client for the amuleapi /api/v0 surface.
//
// - Always sends the session cookie (credentials: "include").
// - Parses the {error:{code,message}} envelope into ApiError.
// - Caches ETags per GET target and revalidates with If-None-Match so a
//   304 short-circuits re-downloading/parsing an unchanged body.
// - Absolute "/api/v0" base: amuleapi serves this frontend on the same origin
//   as the REST + SSE API, so the fixed root always resolves.

import { t } from "./i18n.js";

const BASE = "/api/v0";

export class ApiError extends Error {
  constructor(status, code, message) {
    super(message || code || ("HTTP " + status));
    this.name = "ApiError";
    this.status = status;
    this.code = code || "";
  }
}

// path -> { etag, data }
const etagCache = new Map();

// Optional hook invoked on any 401 so the shell can bounce to login.
let onUnauthorized = null;
export function setUnauthorizedHandler(fn) { onUnauthorized = fn; }

async function request(method, path, { body, useEtag = false } = {}) {
  const url = BASE + "/" + path.replace(/^\//, "");
  const headers = {};
  let cacheKey = null;

  if (useEtag && (method === "GET" || method === "HEAD")) {
    cacheKey = url;
    const cached = etagCache.get(cacheKey);
    if (cached) headers["If-None-Match"] = '"' + cached.etag + '"';
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

  if (resp.status === 401 && onUnauthorized) onUnauthorized();

  // No-body success (e.g. 204).
  if (resp.status === 204) return {};

  const text = await resp.text();
  let payload = null;
  if (text) {
    try { payload = JSON.parse(text); } catch (_) { payload = null; }
  }

  if (!resp.ok) {
    const err = payload && payload.error ? payload.error : {};
    throw new ApiError(resp.status, err.code, err.message);
  }

  if (cacheKey) {
    const etag = (resp.headers.get("ETag") || "").replace(/"/g, "");
    if (etag) etagCache.set(cacheKey, { etag, data: payload });
  }

  return payload === null ? {} : payload;
}

export const api = {
  get: (path, opts) => request("GET", path, { useEtag: true, ...opts }),
  post: (path, body) => request("POST", path, { body }),
  patch: (path, body) => request("PATCH", path, { body }),
  del: (path) => request("DELETE", path),

  // auth
  login: (password) => request("POST", "auth/login", { body: { password } }),
  logout: () => request("POST", "auth/logout"),
  session: () => request("GET", "auth/session"),
};
