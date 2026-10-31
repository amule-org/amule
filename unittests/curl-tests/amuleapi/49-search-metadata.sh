#!/usr/bin/env bash
# Offline metadata search round trip through the real API and daemon.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" != "--fixture-ready" ]; then
	: "${AMULEAPI_BIN:?Set AMULEAPI_BIN}"
	AMULED_BIN=${AMULED_BIN:-$(dirname "$AMULEAPI_BIN")/../amuled}
	exec python3 "$SCRIPT_DIR/../../tests/AllSearchApiSmoke.py" \
		"$AMULED_BIN" "$AMULEAPI_BIN" "$SCRIPT_DIR/49-search-metadata.sh"
fi
TOKEN=$(curl -fsS --max-time 10 -X POST -H 'Content-Type: application/json' \
	-d '{"password":"adminpass"}' "$API/auth/login?include_token=true" | jq -er .token)
AUTH="Authorization: Bearer $TOKEN"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsS --max-time 10 -X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d '{"network":"kad"}' "$API/networks/connect" >/dev/null
# The restored fixture has no form: all six fields must be present and null.
curl -fsS --max-time 10 -H "$AUTH" "$API/search" >"$TMP/list"
jq -e --argjson sid "$ALL_SID" '
	.searches[] | select(.search_id == $sid) | . as $row |
	all(["min_bitrate_kilobits_per_second", "min_duration_seconds", "codec", "title", "album", "artist"][];
		. as $key | ($row | has($key)) and $row[$key] == null)' "$TMP/list" >/dev/null
echo 'PASS: unavailable media filters are always present and null'
for DURATION in 0 3723 86400; do
	FILTERS=$(jq -nc --argjson duration "$DURATION" '{min_bitrate_kilobits_per_second:1000000,
		min_duration_seconds:$duration, codec:"FLAC", title:"曲 AND OR NOT", album:"Test Album", artist:"Test Artist"}')
	BODY=$(jq -nc --argjson filters "$FILTERS" '{query:"offlinemetadataregression",type:"kad"} + $filters')
	curl -fsS --max-time 10 -X POST -H "$AUTH" -H 'Content-Type: application/json' \
		-d "$BODY" "$API/search" >"$TMP/created"
	SID=$(jq -er .search_id "$TMP/created")
	jq -e --argjson filters "$FILTERS" '. as $row | all($filters | keys[]; . as $key | $row[$key] == $filters[$key])' "$TMP/created" >/dev/null
	curl -fsS --max-time 10 -H "$AUTH" "$API/search" >"$TMP/list"
	jq -e --argjson sid "$SID" --argjson filters "$FILTERS" '
		.searches[] | select(.search_id == $sid) | . as $row |
		all($filters | keys[]; . as $key | $row[$key] == $filters[$key])' "$TMP/list" >/dev/null
	curl -fsS --max-time 10 -X DELETE -H "$AUTH" "$API/search/$SID"
done
echo 'PASS: REST -> EC -> core -> EC -> REST preserves every media filter and inclusive bounds'
# A recorded form with defaults reports zero and empty string rather than null.
curl -fsS --max-time 10 -X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d '{"query":"offlinemetadatadefaults","type":"kad"}' "$API/search" >"$TMP/created"
SID=$(jq -er .search_id "$TMP/created")
for ROW in created list; do
	if [ "$ROW" = list ]; then
		curl -fsS --max-time 10 -H "$AUTH" "$API/search" | jq --argjson sid "$SID" '.searches[] | select(.search_id == $sid)' >"$TMP/list"
	fi
	jq -e '.min_bitrate_kilobits_per_second == 0 and .min_duration_seconds == 0 and
		.codec == "" and .title == "" and .album == "" and .artist == ""' "$TMP/$ROW" >/dev/null
done
curl -fsS --max-time 10 -X DELETE -H "$AUTH" "$API/search/$SID"
echo 'PASS: default media filters remain zero and empty strings on recorded forms'

# JSON integers can exceed INT64_MAX; size filters must keep their unsigned value.
curl -fsS --max-time 10 -X POST -H "$AUTH" -H 'Content-Type: application/json' \
	-d '{"query":"offlineunsignedsizes","type":"kad","min_size_bytes":9223372036854775808,"max_size_bytes":18446744073709549568}' "$API/search" >"$TMP/created"
SID=$(jq -er .search_id "$TMP/created")
for ROW in created list; do
	if [ "$ROW" = list ]; then
		curl -fsS --max-time 10 -H "$AUTH" "$API/search" | jq --argjson sid "$SID" '.searches[] | select(.search_id == $sid)' >"$TMP/list"
	fi
	jq -e '.min_size_bytes == 9223372036854775808 and .max_size_bytes == 18446744073709549568' "$TMP/$ROW" >/dev/null
done
curl -fsS --max-time 10 -X DELETE -H "$AUTH" "$API/search/$SID"
echo 'PASS: unsigned 64-bit size filters retain their value in creation and listing rows'
