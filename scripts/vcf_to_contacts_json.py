#!/usr/bin/env python3
"""Convert a vCard (.vcf) file to contacts.json for CrossPoint Reader.

Usage:
    python3 vcf_to_contacts_json.py input.vcf [output.json]

If output path is omitted, writes to contacts.json in the current directory.
Output is a sorted JSON array with compact keys:
    [{"n": "Name", "p": ["+31..."], "e": ["a@b.com"], "o": "Company"}, ...]

Copy the output file to /.crosspoint/contacts.json on the e-reader's SD card.
"""

from __future__ import annotations

import json
import re
import sys


def unfold_lines(lines: list[str]) -> list[str]:
    """Unfold vCard continuation lines (RFC 2425: lines starting with space/tab)."""
    result: list[str] = []
    for line in lines:
        if line and line[0] in (" ", "\t") and result:
            result[-1] += line[1:]
        else:
            result.append(line)
    return result


def extract_value(line: str) -> str:
    """Extract the value part after the first unescaped colon."""
    # Property lines look like: PROP;PARAM=X:value or PROP:value
    colon = line.find(":")
    if colon < 0:
        return ""
    return line[colon + 1 :].strip()


def clean_phone(raw: str) -> str:
    """Strip tel: prefix and normalize whitespace."""
    val = raw.strip()
    if val.lower().startswith("tel:"):
        val = val[4:]
    # Remove internal whitespace/dashes for compact display
    return val.strip()


def clean_email(raw: str) -> str:
    """Strip mailto: prefix."""
    val = raw.strip()
    if val.lower().startswith("mailto:"):
        val = val[7:]
    return val.strip()


def name_from_n_field(val: str) -> str:
    """Build display name from structured N field: last;first;middle;prefix;suffix."""
    parts = val.split(";")
    last = parts[0].strip() if len(parts) > 0 else ""
    first = parts[1].strip() if len(parts) > 1 else ""
    if first and last:
        return f"{first} {last}"
    return first or last


def parse_vcf(path: str) -> list[dict]:
    """Parse a vCard file and return a list of contact dicts."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw_lines = [line.rstrip("\r\n") for line in f]

    lines = unfold_lines(raw_lines)

    contacts: list[dict] = []
    in_card = False
    fn = ""
    n_field = ""
    phones: list[str] = []
    emails: list[str] = []
    org = ""

    for line in lines:
        upper = line.upper()

        if upper == "BEGIN:VCARD":
            in_card = True
            fn = ""
            n_field = ""
            phones = []
            emails = []
            org = ""
            continue

        if upper == "END:VCARD":
            in_card = False
            name = fn or name_from_n_field(n_field)
            if name:
                contact: dict = {"n": name}
                if phones:
                    contact["p"] = phones
                if emails:
                    contact["e"] = emails
                if org:
                    contact["o"] = org
                contacts.append(contact)
            continue

        if not in_card:
            continue

        # Skip PHOTO and its potential continuation (already unfolded)
        if upper.startswith("PHOTO"):
            continue

        # Property name is everything before the first ; or :
        prop_end = len(line)
        for i, c in enumerate(line):
            if c in (";", ":"):
                prop_end = i
                break
        prop = line[:prop_end].upper()

        if prop == "FN":
            fn = extract_value(line)
        elif prop == "N":
            n_field = extract_value(line)
        elif prop == "TEL":
            phone = clean_phone(extract_value(line))
            if phone and phone not in phones:
                phones.append(phone)
        elif prop == "EMAIL":
            email = clean_email(extract_value(line))
            if email and email not in emails:
                emails.append(email)
        elif prop == "ORG":
            org = extract_value(line).rstrip(";").strip()

    return contacts


def sort_key(contact: dict) -> tuple[int, str]:
    """Sort contacts: A-Z first, then # for non-alpha names."""
    name = contact["n"]
    first = name[0].upper() if name else ""
    if first.isascii() and first.isalpha():
        return (0, name.lower())
    return (1, name.lower())


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else "contacts.json"

    contacts = parse_vcf(input_path)
    contacts.sort(key=sort_key)

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(contacts, f, ensure_ascii=False, separators=(",", ":"))

    print(f"Wrote {len(contacts)} contacts to {output_path}")

    # Show size info
    import os

    size = os.path.getsize(output_path)
    print(f"File size: {size:,} bytes ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
