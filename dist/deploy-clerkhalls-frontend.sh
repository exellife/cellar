#!/usr/bin/env bash
# Redeploy the ClerkHalls SPA to the Oracle A1 (clerkhalls.svngn.com), atomically.
#
# clerkhalls is a PRIVATE org tool (login-gated, admin-provisioned, unlinked) and must
# stay out of search engines. Its robots.txt (Disallow: /) is INJECTED here rather than
# relied upon from the build: the frontend's `pnpm build` uses emptyOutDir and wipes
# dist/, so any robots.txt placed in dist/ vanishes on the next rebuild. Injecting at
# deploy time makes the crawl-block survive every frontend rebuild.
#
#   dist/deploy-clerkhalls-frontend.sh [dist-dir]
#
# Prior public/ is kept as public.old on the box for one-mv rollback.
set -euo pipefail

DIST="${1:-/home/racytech/workspace/frontend-apps/apps/clerkhalls/dist}"
SSH=oracle-a1
HOST=clerkhalls.svngn.com
APP=/var/lib/cellar/apps/$HOST

[ -f "$DIST/index.html" ] || { echo "no build at $DIST — run: pnpm --filter clerkhalls build" >&2; exit 1; }

echo "== stage dist -> $SSH =="
rsync -az --delete "$DIST/" "$SSH:ch-public-new/"

echo "== atomic swap + inject robots.txt (Disallow) =="
ssh "$SSH" "sudo bash -s" <<REMOTE
set -e
NEW=$APP/public.new
rm -rf "\$NEW" "$APP/public.old"
cp -a /home/ubuntu/ch-public-new "\$NEW"
printf 'User-agent: *\nDisallow: /\n' > "\$NEW/robots.txt"
chown -R cellar:cellar "\$NEW"
mv "$APP/public" "$APP/public.old" && mv "\$NEW" "$APP/public"
echo swapped
REMOTE

echo "== verify live =="
curl -s "https://$HOST/" | grep -oE 'assets/index-[^"]+\.(js|css)'
curl -s -o /dev/null -w "health     HTTP %{http_code} TLS:%{ssl_verify_result}\n" "https://$HOST/health"
curl -s -o /dev/null -w "robots.txt HTTP %{http_code} type=%{content_type}\n"     "https://$HOST/robots.txt"
echo "rollback: mv $APP/public.old $APP/public  (on $SSH, as root)"
