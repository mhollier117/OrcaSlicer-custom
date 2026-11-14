# The revision of this fork's own releases, on top of the upstream version in version.inc.
# Raise it by one for a release that installed builds must be offered as an update while the upstream
# version stays the same. It never needs to go back; keep it below 100 (the version parser folds it
# into the patch number as patch * 100 + revision).
set(ORCA_CUSTOM_REVISION "2")
