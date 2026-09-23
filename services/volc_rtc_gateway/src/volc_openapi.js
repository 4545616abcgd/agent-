/**
 * OpenAPI signing flow ported from VolcEngine's rtc-aigc-embedded-demo.
 * Copyright 2025 Beijing Volcano Engine Technology Co., Ltd.
 * Licensed under MIT; see THIRD_PARTY_NOTICES.md.
 */

const crypto = require("node:crypto");

// The configured AppID belongs to the "Realtime Conversational AI" product.
// The 2025-06-01 API is for the separate "AI Audio/Video Interaction" product.
const RTC_API_VERSION = "2024-12-01";
const RTC_ENDPOINT = "https://rtc.volcengineapi.com";
const RTC_REGION = "cn-north-1";
const RTC_SERVICE = "rtc";

function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function hmac(key, value) {
  return crypto.createHmac("sha256", key).update(value).digest();
}

function createSignedRequest(config, action, body, now = new Date()) {
  const bodyText = JSON.stringify(body);
  const contentHash = sha256(bodyText);
  const xDate = now.toISOString().replace(/[:-]|\.\d{3}/g, "");
  const canonicalQuery = `Action=${encodeURIComponent(
    action
  )}&Version=${encodeURIComponent(RTC_API_VERSION)}`;
  const canonicalHeaders = [
    "content-type:application/json",
    `host:${new URL(RTC_ENDPOINT).host}`,
    `x-content-sha256:${contentHash}`,
    `x-date:${xDate}`,
    "",
  ].join("\n");
  const signedHeaders = "content-type;host;x-content-sha256;x-date";
  const canonicalRequest = [
    "POST",
    "/",
    canonicalQuery,
    canonicalHeaders,
    signedHeaders,
    contentHash,
  ].join("\n");
  const date = xDate.slice(0, 8);
  const credentialScope = `${date}/${RTC_REGION}/${RTC_SERVICE}/request`;
  const stringToSign = [
    "HMAC-SHA256",
    xDate,
    credentialScope,
    sha256(canonicalRequest),
  ].join("\n");
  let signature = Buffer.from(config.secretAccessKey, "utf8");
  for (const value of [date, RTC_REGION, RTC_SERVICE, "request", stringToSign]) {
    signature = hmac(signature, value);
  }
  const authorization =
    `HMAC-SHA256 Credential=${config.accessKeyId}/${credentialScope}, ` +
    `SignedHeaders=${signedHeaders}, Signature=${signature.toString("hex")}`;
  return {
    url: `${RTC_ENDPOINT}/?${canonicalQuery}`,
    options: {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Host: new URL(RTC_ENDPOINT).host,
        "X-Content-Sha256": contentHash,
        "X-Date": xDate,
        Authorization: authorization,
      },
      body: bodyText,
      signal: AbortSignal.timeout(15000),
    },
  };
}

function createOpenApiInvoker(config, fetchImpl = fetch, clock = () => new Date()) {
  return async (action, body) => {
    const request = createSignedRequest(config, action, body, clock());
    const response = await fetchImpl(request.url, request.options);
    const text = await response.text();
    let result;
    try {
      result = JSON.parse(text);
    } catch {
      throw new Error(`RTC OpenAPI returned invalid JSON (HTTP ${response.status})`);
    }
    if (!response.ok) {
      throw new Error(`RTC OpenAPI HTTP ${response.status}`);
    }
    return result;
  };
}

module.exports = {
  RTC_API_VERSION,
  createOpenApiInvoker,
  createSignedRequest,
};
