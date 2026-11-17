# The name at the front of a download's file name (sourced: dist_name <version>). Releases and test builds:
# "ReAwakening". Nightly and experimental builds keep "LAS Recomp" (GitHub: LAS.Recomp-...): every installed one finds
# its updates by that prefix (runtime/ui/update_tags.h newest_rolling). Inside the files nothing changes: lasrecomp,
# the data folder, the Windows zip's "LAS Recomp" folder (runtime/update.cpp unpacks that one).
dist_name() {
    case $1 in nightly-* | experimental-*) echo "LAS Recomp" ;; *) echo "ReAwakening" ;; esac
}
