Release notes shown in the app («تازه‌های نسخهٔ …» on the updates page).

One pair of files per version, plain text, a few short lines:

    <version>.fa.md     Persian (shown first to Persian-language users)
    <version>.en.md     English

`packaging/make-update-manifest.py` puts them into `update.json`; a missing
file is an empty note. Keep each under 16 000 characters.
