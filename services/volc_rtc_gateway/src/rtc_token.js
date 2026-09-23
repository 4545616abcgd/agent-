/**
 * RTC token wire format derived from VolcEngine's rtc-aigc-demo token.js.
 * Copyright 2025 Beijing Volcano Engine Technology Co., Ltd.
 * Licensed under BSD-3-Clause; see THIRD_PARTY_NOTICES.md.
 */

const crypto = require("node:crypto");

const VERSION = "001";
const PRIV_PUBLISH_STREAM = 0;
const PRIV_PUBLISH_AUDIO_STREAM = 1;
const PRIV_PUBLISH_VIDEO_STREAM = 2;
const PRIV_PUBLISH_DATA_STREAM = 3;
const PRIV_SUBSCRIBE_STREAM = 4;

class ByteWriter {
  constructor() {
    this.parts = [];
  }

  putUint16(value) {
    const buffer = Buffer.allocUnsafe(2);
    buffer.writeUInt16LE(value);
    this.parts.push(buffer);
    return this;
  }

  putUint32(value) {
    const buffer = Buffer.allocUnsafe(4);
    buffer.writeUInt32LE(value >>> 0);
    this.parts.push(buffer);
    return this;
  }

  putBytes(value) {
    if (value.length > 0xffff) {
      throw new Error("RTC token field is too large");
    }
    return this.putUint16(value.length).append(value);
  }

  putString(value) {
    return this.putBytes(Buffer.from(value, "utf8"));
  }

  append(value) {
    this.parts.push(value);
    return this;
  }

  pack() {
    return Buffer.concat(this.parts);
  }
}

function createRtcToken({ appId, appKey, roomId, userId, expiresAt }) {
  if (!appId || !appKey || !roomId || !userId) {
    throw new Error("RTC token requires appId, appKey, roomId and userId");
  }
  const now = Math.floor(Date.now() / 1000);
  const privileges = new Map([
    [PRIV_PUBLISH_STREAM, 0],
    [PRIV_PUBLISH_AUDIO_STREAM, 0],
    [PRIV_PUBLISH_VIDEO_STREAM, 0],
    [PRIV_PUBLISH_DATA_STREAM, 0],
    [PRIV_SUBSCRIBE_STREAM, 0],
  ]);
  const message = new ByteWriter()
    .putUint32(crypto.randomInt(0, 0x100000000))
    .putUint32(now)
    .putUint32(expiresAt)
    .putString(roomId)
    .putString(userId)
    .putUint16(privileges.size);
  for (const [privilege, privilegeExpiry] of privileges) {
    message.putUint16(privilege).putUint32(privilegeExpiry);
  }
  const packedMessage = message.pack();
  const signature = crypto
    .createHmac("sha256", appKey)
    .update(packedMessage)
    .digest();
  const content = new ByteWriter()
    .putBytes(packedMessage)
    .putBytes(signature)
    .pack();
  return `${VERSION}${appId}${content.toString("base64")}`;
}

module.exports = { createRtcToken };
