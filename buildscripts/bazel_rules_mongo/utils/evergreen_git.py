import os
from functools import cache
from typing import Dict, List, Optional

import yaml
from git import Remote, Repo


@cache
def get_expansions(expansions_file: str) -> Dict[str, any]:
    if not expansions_file:
        return None

    if not os.path.exists(expansions_file):
        raise RuntimeError(f"Expansions file not found at {expansions_file}")

    with open(expansions_file, "r") as file:
        return yaml.safe_load(file)


def get_mongodb_remote(repo: Repo) -> Remote:
    remotes = repo.remotes
    picked_remote = None
    for remote in remotes:
        url = remote.url
        # local repository pointing to a local dir
        remote_prefixes = ("http://", "https://", "ssh://", "git@")
        if not any(url.startswith(prefix) for prefix in remote_prefixes):
            continue
        # get rid of .git suffix if it exists
        if url.endswith(".git"):
            url = url[:-4]

        # all other remote urls should end with owner/project
        parts = url.split("/")
        assert len(parts) >= 2, f"Unexpected git remote url: {url}"
        owner = parts[-2].split(":")[-1]

        if owner in ("10gen", "mongodb", "evergreen-ci", "mongodb-ets", "realm", "mongodb-js"):
            picked_remote = remote
            print(f"Selected remote: {remote.url}")
            break

    if picked_remote is None:
        print(
            "Could not find remote from any mongodb github org, falling back to the first remote found"
        )
        picked_remote = next(repo.remotes)

    if picked_remote is None:
        raise RuntimeError("Could not find valid remote")

    return picked_remote


def get_remote_branch_ref(repo: Repo, branch: str = None) -> str:
    # If branch is not specified, default to master or main
    if branch is None:
        for branch in repo.branches:
            if branch.name in ("main", "master"):
                branch = branch.name
                break

        if branch is None:
            raise RuntimeError("Could not infer correct branch name")

    # pick a remote from a mongodb org
    picked_remote = get_mongodb_remote(repo)

    # Get latest head of remote branch
    remote_branch = repo.refs[f"{picked_remote.name}/{branch}"]
    remote_head = remote_branch.commit

    # Get the current HEAD commit
    local_head = repo.head.commit

    # Return the best common ancestor commit between current head and remote repo
    return repo.git.merge_base(local_head.hexsha, remote_head.hexsha)


def get_new_files(expansions_file: str = None, branch: str = None) -> List[str]:
    # docs on the diff-filter are here https://www.kernel.org/pub/software/scm/git/docs/git-diff.html
    # This gets added, renamed, and copied files from the git diff.
    return get_changed_files(expansions_file, branch, diff_filter="ARC")


@cache
def get_diff_revision(expansions_file: str = None, branch: str = None) -> str:
    in_ci = expansions_file is not None
    repo = Repo()

    if not in_ci:
        diff_commit = get_remote_branch_ref(repo, branch)
    else:
        expansions = get_expansions(expansions_file)
        if expansions.get("is_patch", None):
            # patches from the cli have the changes uncommited, we need to add them to git for git diff to work
            # we add the files in github patches as well to make it fail consistently if new files
            # are generated in CI before this point.
            repo.git.execute(["git", "add", "."])
            diff_commit = expansions.get("revision")
        else:
            # in waterfall runs we just want to compare to the previous commit
            diff_commit = repo.git.execute(["git", "rev-parse", "HEAD^1"])

    assert diff_commit, "ERROR: not able to obtain diff commit"
    return diff_commit


def get_changed_files(
    expansions_file: str = None, branch: str = None, diff_filter: str = "d"
) -> List[str]:
    diff_commit = get_diff_revision(expansions_file, branch)
    repo = Repo()

    output = repo.git.execute(
        ["git", "diff", "--name-only", f"--diff-filter={diff_filter}", diff_commit]
    )
    files = output.split("\n")
    return [file for file in files if file]


def get_file_at_revision(file: str, revision: str) -> Optional[str]:
    repo = Repo()
    try:
        return repo.git.execute(["git", "show", f"{revision}:{file}"])
    except Exception as ex:
        # If the file did not exist in the previous revision return None
        if f"path '{file}' does not exist in '{revision}'" in str(ex):
            return None
        raise ex
