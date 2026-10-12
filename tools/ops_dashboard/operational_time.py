"""Explicit airport-timezone parsing for operational schedules and events."""
from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

UTC = timezone.utc


class OperationalTimeError(ValueError):
    pass


def zone(name: str) -> ZoneInfo:
    if not isinstance(name, str) or not name.strip():
        raise OperationalTimeError("An IANA airport timezone is required.")
    try:
        return ZoneInfo(name)
    except (ZoneInfoNotFoundError, ValueError) as exc:
        raise OperationalTimeError(f"Unknown IANA timezone {name!r}.") from exc


def _parse_wall(value: str, fmt: str | None, operational_date: str | None) -> tuple[datetime, bool]:
    value = value.strip()
    if fmt:
        try:
            parsed = datetime.strptime(value, fmt)
        except ValueError as exc:
            raise OperationalTimeError(f"Timestamp does not match configured format {fmt!r}.") from exc
        has_date = any(token in fmt for token in ("%Y", "%y", "%m", "%d", "%j"))
        if not has_date and operational_date:
            parsed = datetime.combine(date.fromisoformat(operational_date), parsed.time())
        return parsed, not has_date
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed, False
    except ValueError:
        if operational_date:
            for clock_format in ("%H:%M:%S", "%H:%M"):
                try:
                    clock = datetime.strptime(value, clock_format).time()
                    day = date.fromisoformat(operational_date)
                    return datetime.combine(day, clock), True
                except ValueError:
                    continue
        raise OperationalTimeError("Expected an ISO timestamp or a configured timestamp format.") from None


def parse_utc(value: object, timezone_name: str, *, timestamp_format: str | None = None,
              operational_date: str | None = None, fold: int | None = None) -> datetime:
    """Parse a timestamp into aware UTC; naive values use the airport's IANA zone.

    Ambiguous and nonexistent local wall times require an explicit UTC offset. A
    time-only value is anchored to operational_date and reports rollover
    separately to the schedule normalizer.
    """
    if value is None or not str(value).strip():
        raise OperationalTimeError("Timestamp is missing.")
    parsed, _time_only = _parse_wall(str(value), timestamp_format, operational_date)
    if parsed.tzinfo is not None:
        return parsed.astimezone(UTC)
    tz = zone(timezone_name)
    candidates = []
    for candidate_fold in (0, 1):
        candidate = parsed.replace(tzinfo=tz, fold=candidate_fold)
        roundtrip = candidate.astimezone(UTC).astimezone(tz).replace(tzinfo=None)
        if roundtrip == parsed:
            candidates.append(candidate)
    unique_offsets = {candidate.utcoffset() for candidate in candidates}
    if not candidates:
        raise OperationalTimeError("Local timestamp falls in a nonexistent daylight-saving time interval.")
    if len(unique_offsets) > 1:
        if fold not in (0, 1):
            raise OperationalTimeError("Local timestamp is ambiguous at a daylight-saving boundary; provide a UTC offset or fold.")
        selected = parsed.replace(tzinfo=tz, fold=fold)
        if selected.astimezone(UTC).astimezone(tz).replace(tzinfo=None) != parsed:
            raise OperationalTimeError("Selected daylight-saving fold is invalid for this local timestamp.")
        return selected.astimezone(UTC)
    return candidates[0].astimezone(UTC)


def is_time_only(value: object, timestamp_format: str | None = None) -> bool:
    if not isinstance(value, str) or not value.strip():
        return False
    if timestamp_format:
        return not any(token in timestamp_format for token in ("%Y", "%y", "%m", "%d", "%j"))
    try:
        datetime.fromisoformat(value.strip().replace("Z", "+00:00"))
        return False
    except ValueError:
        pass
    return any(_matches_clock(value.strip(), fmt) for fmt in ("%H:%M", "%H:%M:%S"))


def _matches_clock(value: str, fmt: str) -> bool:
    try:
        datetime.strptime(value, fmt)
        return True
    except ValueError:
        return False


def local_clock(value: str, timezone_name: str) -> time:
    return parse_utc(value, timezone_name).astimezone(zone(timezone_name)).timetz().replace(tzinfo=None)


def iso_utc(value: datetime) -> str:
    if value.tzinfo is None:
        raise OperationalTimeError("Normalized UTC values must be timezone-aware.")
    return value.astimezone(UTC).isoformat(timespec="seconds").replace("+00:00", "Z")


def parse_date(value: object) -> str:
    try:
        return date.fromisoformat(str(value)).isoformat()
    except ValueError as exc:
        raise OperationalTimeError("Operational date must use YYYY-MM-DD.") from exc


def utc_now() -> str:
    return iso_utc(datetime.now(UTC))
