_UMLAUTS = str.maketrans({
    "ä": "ae", "ö": "oe", "ü": "ue", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue", "ß": "ss",
    "–": "-", "—": "-", "„": '"', "“": '"', "”": '"', "’": "'",
})


def to_sign_text(value):
    """The sign's 5x7 font is ASCII only: spell out umlauts, drop the rest."""
    value = str(value or "").translate(_UMLAUTS)
    return value.encode("ascii", "ignore").decode("ascii").strip()
