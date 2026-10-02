// Xac thuc thiet bi (ESP32) va tien ich lien quan - khong dung thu vien ngoai,
// chi Web Crypto co san tren Cloudflare Workers.

function toHex(buffer) {
  return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Bam device_key bang SHA-256 (co "pepper" bi mat cua worker tron vao truoc khi
// bam) - KHONG luu device_key dang plaintext trong D1. Day khong phai mat khau
// nguoi dung (khong can bcrypt/argon2 cham), la 1 chuoi bi mat co do ngau nhien
// cao do firmware tu sinh/duoc cap luc san xuat, nen SHA-256 + pepper la du.
export async function hashDeviceKey(deviceKey, pepper) {
  const data = new TextEncoder().encode(`${pepper}:${deviceKey}`);
  const digest = await crypto.subtle.digest('SHA-256', data);
  return toHex(digest);
}

// So sanh 2 chuoi hex ve mat thoi gian co dinh (khong short-circuit theo do
// dai/ky tu dau tien khac nhau) - han che kieu tan cong do thoi gian phan hoi.
export function timingSafeEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

export async function verifyDeviceKey(deviceKey, pepper, storedHash) {
  if (!deviceKey || !storedHash) return false;
  const computed = await hashDeviceKey(deviceKey, pepper);
  return timingSafeEqual(computed, storedHash);
}

export function randomToken(byteLength = 16) {
  const bytes = new Uint8Array(byteLength);
  crypto.getRandomValues(bytes);
  return toHex(bytes.buffer);
}

export function isValidDeviceId(id) {
  return typeof id === 'string' && /^[A-Za-z0-9_-]{3,40}$/.test(id);
}
