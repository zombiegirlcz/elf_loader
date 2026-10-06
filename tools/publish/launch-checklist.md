# Launch checklist — elf_loader v0.1

Everything text/code side is ready. This file is the publish runbook; the
actual posting is done through the Playwright MCP tools once you supply
**cookies** (Reddit, HN) and the **video URL**.

## Ready artifacts

| artifact | path / URL |
|---|---|
| Main README (English) | `README.md` → https://github.com/zombiegirlcz/elf_loader |
| How it works | `docs/how-it-works.md` |
| Blog post (for HN) | `docs/blog-hn.md` |
| Social drafts | `docs/social-posts.md` |
| Bug report template | `.github/ISSUE_TEMPLATE/bug_report.yml` |
| "It works" template | `.github/ISSUE_TEMPLATE/working_binary.yml` |
| Release + binaries | https://github.com/zombiegirlcz/elf_loader/releases/tag/v0.1 |
| Demo script | `tools/demo-env.zsh` |

## What I need from you

1. **Video URL** — the screen recording, uploaded somewhere public
   (YouTube / asciinema / a gist-hosted mp4). I'll drop it into:
   - `README.md` (the `> _A 15-second screen recording …_` placeholder)
   - every post in `docs/social-posts.md` (`<VIDEO_URL>`)
2. **Reddit cookies** — exported `Cookie:` header for `reddit.com` (logged in).
3. **HN cookies** — exported `Cookie:` header for `news.ycombinator.com`
   (logged in as the account you want to post from).
4. **Blog URL** (optional) — if you want the HN link to point at a real blog
   (dev.to / GitHub Pages) instead of the repo. Otherwise HN points at the repo.

## Publish order (when video + cookies are in)

1. **Fill placeholders** — replace `<VIDEO_URL>`, `<BLOG_URL>`, `<REPO>` in
   `docs/social-posts.md` and add the video link to `README.md`; commit + push.
2. **Blog post** — if using a blog, publish `docs/blog-hn.md` first and get its
   URL for the HN submission.
3. **GitHub Discussions** — create the pinned announcement thread.
4. **Reddit** (via Playwright, one at a time, spaced out):
   - r/linux_on_android  — main post
   - r/termux            — proot-comparison variant
   - r/unixporn          — zsh + starship angle
   - *(skip r/androiddev — off-topic, gets removed)*
5. **Hacker News** — `Show HN` with the blog URL, then immediately post the
   author first-comment from `docs/social-posts.md`.
6. **X / Twitter** — the 5-tweet thread.

## Playwright approach

The Playwright MCP tools drive a real browser. Flow per site:

1. `playwright_browser_navigate` to the submit page
   (e.g. `https://www.reddit.com/r/linux_on_android/submit`).
2. Inject cookies (via `playwright_browser_run_code_unsafe` —
   `context.addCookies([...])` — or by setting them through
   `document.cookie` where possible; Reddit needs httpOnly cookies so it has
   to go through the context).
3. `playwright_browser_snapshot` to get element refs, then
   `playwright_browser_fill_form` / `playwright_browser_type` for title + body,
   `playwright_browser_click` on Submit.
4. `playwright_browser_take_screenshot` as a receipt.

Same for HN (`https://news.ycombinator.com/submit`) and X.

## Caveats

- Reddit/HN rate-limit new-account posts and flag suspicious automation. Post
  **manually spaced**, from an account with some history, or expect removals.
- Don't post to all subreddits at once — space them across a day.
- HN dislikes link-drops; the author first-comment matters more than the URL.