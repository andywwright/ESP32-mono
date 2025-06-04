# Loop over every submodule still tracked in index
git ls-files --stage | grep ^160000 | cut -f2- | while read path; do
    echo "Removing submodule entry: $path"
    git rm --cached "$path"
done