#!/usr/bin/env python3
"""Tao nhan QR de in dan len may (thay cho viec go tay Device ID khi them
may tren web). Dung khi co may moi (ID khac, lay theo dia chi MAC ESP32 -
xem man hinh "Ket noi" tren HMI hoac o "May dang dieu khien" tren web).

Cai dat:  pip install "qrcode[pil]"
Chay:     python3 generate_label.py MAP-XXXXXXXXXXXX

Ket qua (trong cung thu muc nay):
  <ID>-qr-only.png   - chi rieng ma QR, dung khi tu dat vao mau tem khac
  <ID>-nhan-in.png   - nhan day du (QR + chu ID doc duoc bang mat), in
                        thang duoc, khuyen nghi dung cai nay
"""
import sys
from pathlib import Path

import qrcode
from qrcode.constants import ERROR_CORRECT_H
from PIL import Image, ImageDraw, ImageFont

DEVICE_ID_PREFIX = "MAP-"


def load_font(paths, size):
    for path in paths:
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def generate(device_id: str, out_dir: Path) -> None:
    device_id = device_id.strip().upper()
    if not device_id.startswith(DEVICE_ID_PREFIX) or len(device_id) != 16:
        raise SystemExit(
            f"ID khong dung dinh dang (vd MAP-441BF6E051D0), nhan duoc: {device_id!r}"
        )

    qr = qrcode.QRCode(
        # 30% du thua - tem dan NGOAI may de tray xuoc/bam bui theo thoi
        # gian, can do ben cao nhat trong 4 muc ma chuan QR ho tro.
        error_correction=ERROR_CORRECT_H,
        box_size=20,
        border=4,
    )
    qr.add_data(device_id)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")

    qr_only_path = out_dir / f"{device_id}-qr-only.png"
    qr_img.save(qr_only_path)

    pad = 40
    qr_size = qr_img.size[0]
    label_w = qr_size + pad * 2
    label_h = qr_size + pad * 2 + 130
    label = Image.new("RGB", (label_w, label_h), "white")
    label.paste(qr_img, (pad, pad + 70))

    draw = ImageDraw.Draw(label)
    title_font = load_font(
        [
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        ],
        34,
    )
    id_font = load_font(
        [
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf",
        ],
        40,
    )

    title = "MAYAP - Quet de them thiet bi"
    tb = draw.textbbox((0, 0), title, font=title_font)
    draw.text(((label_w - (tb[2] - tb[0])) / 2, 22), title, font=title_font, fill="black")

    idb = draw.textbbox((0, 0), device_id, font=id_font)
    draw.text(
        ((label_w - (idb[2] - idb[0])) / 2, pad + 70 + qr_size + 20),
        device_id,
        font=id_font,
        fill="black",
    )

    label_path = out_dir / f"{device_id}-nhan-in.png"
    label.save(label_path, dpi=(300, 300))

    print(f"Da tao: {qr_only_path}")
    print(f"Da tao: {label_path}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"Dung: python3 {sys.argv[0]} MAP-XXXXXXXXXXXX")
    generate(sys.argv[1], Path(__file__).resolve().parent)
