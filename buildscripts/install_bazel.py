#!/usr/bin/env python3

import argparse
import hashlib
import os
import platform
import shutil
import stat
import sys
import time
import urllib.request

_S3_HASH_MAPPING = {
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-darwin-amd64": "f2ba5f721a995b54bab68c6b76a340719888aa740310e634771086b6d1528ecd",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-darwin-arm64": "69fa21cd2ccffc2f0970c21aa3615484ba89e3553ecce1233a9d8ad9570d170e",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-linux-amd64": "d28b588ac0916abd6bf02defb5433f6eddf7cba35ffa808eabb65a44aab226f7",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-linux-arm64": "861a16ba9979613e70bd3d2f9d9ab5e3b59fe79471c5753acdc9c431ab6c9d94",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-windows-amd64.exe": "d04555245a99dfb628e33da24e2b9198beb8f46d7e7661c313eb045f6a59f5e4",
}

BUILDOZER_RELEASE_URL = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/"


def determine_platform():
    syst = platform.system()
    pltf = None
    if syst == "Darwin":
        pltf = "darwin"
    elif syst == "Windows":
        pltf = "windows"
    elif syst == "Linux":
        pltf = "linux"
    else:
        return None
    return pltf


def determine_architecture():
    arch = None
    machine = platform.machine()
    if machine in ("AMD64", "x86_64"):
        arch = "amd64"
    elif machine in ("arm", "arm64", "aarch64"):
        arch = "arm64"
    else:
        return None

    return arch


def _download_path_with_retry(*args, **kwargs):
    for i in range(5):
        try:
            return urllib.request.urlretrieve(*args, **kwargs)
        except Exception as e:
            print(f"Download failed: {e}")
            if i == 4:
                raise
            print("Retrying download...")
            time.sleep(3)
            continue


def _sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


def _verify_s3_hash(s3_path: str, local_path: str) -> None:
    hash_string = _sha256_file(local_path)
    if hash_string != _S3_HASH_MAPPING[s3_path]:
        raise Exception(
            f"Hash mismatch for {s3_path}, expected {_S3_HASH_MAPPING[s3_path]} but got {hash_string}"
        )


def install_buildozer(download_location: str = "./"):
    operating_system = determine_platform()
    architechture = determine_architecture()
    if operating_system is None or architechture is None:
        print("Unsupported OS for buildozer, not installing.")
        return None

    if operating_system == "windows" and architechture == "arm64":
        print("There are no published arm windows releases for buildifier.")
        return None

    extension = ".exe" if operating_system == "windows" else ""
    binary_name = f"buildozer-{operating_system}-{architechture}{extension}"
    url = f"{BUILDOZER_RELEASE_URL}{binary_name}"

    file_location = os.path.join(download_location, f"buildozer{extension}")
    _download_path_with_retry(url, file_location)
    os.chmod(file_location, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    return file_location


def install_bazel(binary_directory: str) -> str:
    install_buildozer(binary_directory)
    normalized_arch = (
        platform.machine().lower().replace("aarch64", "arm64").replace("x86_64", "amd64")
    )
    normalized_os = sys.platform.replace("win32", "windows").replace("darwin", "macos")
    is_bazelisk_supported = normalized_arch not in ["ppc64le", "s390x"]
    binary_filename = "bazelisk"
    binary_path = os.path.join(binary_directory, binary_filename)
    if os.path.exists(binary_path):
        print(f"{binary_filename} already exists ({binary_path}), skipping download")
        _set_bazel_permissions(binary_path)
        return binary_path

    if is_bazelisk_supported:
        print(f"Downloading {binary_filename}...")
        ext = ".exe" if normalized_os == "windows" else ""
        os_str = normalized_os.replace("macos", "darwin")
        s3_path = f"https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-{os_str}-{normalized_arch}{ext}"
        _download_path_with_retry(s3_path, binary_path)
        _verify_s3_hash(s3_path, binary_path)
        print(f"Downloaded {binary_filename} to {binary_path}")

    else:
        print("Using bazel/bazelisk.py on unsupported platform.")
        repo_dir = os.path.dirname(os.path.dirname(__file__))
        bazelisk_py = os.path.join(repo_dir, "bazel", "bazelisk.py")
        shutil.copyfile(bazelisk_py, binary_path)

    _set_bazel_permissions(binary_path)
    return binary_path


def _set_bazel_permissions(binary_path: str) -> None:
    # Bazel is a self-extracting zip launcher and needs read perms on the executable to read the zip from itself.
    perms = (
        stat.S_IXUSR
        | stat.S_IXGRP
        | stat.S_IXOTH
        | stat.S_IRUSR
        | stat.S_IRGRP
        | stat.S_IROTH
        | stat.S_IWUSR
        | stat.S_IWGRP
    )
    os.chmod(binary_path, perms)


def create_bazel_to_bazelisk_symlink(binary_directory: str) -> str:
    bazel_symlink = os.path.join(
        binary_directory, "bazel.exe" if sys.platform == "win32" else "bazel"
    )
    if os.path.exists(bazel_symlink):
        print(f"Symlink {bazel_symlink} already exists, skipping symlink creation")
        return bazel_symlink

    os.symlink(os.path.join(binary_directory, "bazelisk"), bazel_symlink)
    print(f"Symlinked bazel to {bazel_symlink}")
    return bazel_symlink


def main():
    arg_parser = argparse.ArgumentParser()
    arg_parser.add_argument("--add-bazel-symlink", type=bool, default=True)
    args = arg_parser.parse_args()

    binary_directory = os.path.expanduser("~/.local/bin")
    if not os.path.exists(binary_directory):
        os.makedirs(binary_directory)

    install_bazel(binary_directory)

    if args.add_bazel_symlink:
        symlink_path = create_bazel_to_bazelisk_symlink(binary_directory)

        path_misconfigured = False
        evaluated_path = shutil.which(os.path.basename(symlink_path))
        if evaluated_path is None:
            print(
                "Warning: bazel is not in the PATH. Please add ~/.local/bin to your PATH or call it with the absolute path."
            )
            path_misconfigured = True
        elif os.path.abspath(evaluated_path) != os.path.abspath(symlink_path):
            print(
                f"Warning: the bazel installed ({evaluated_path}) doesn't match the bazel in your path"
            )
            path_misconfigured = True

        if path_misconfigured:
            abs_binary_directory = os.path.abspath(binary_directory)
            if sys.platform == "win32":
                print("To add it to your PATH, run: \n")
                print(
                    f'[Environment]::SetEnvironmentVariable("Path", "{abs_binary_directory};" + $env:Path, "Machine")'
                )
                print("refreshenv")
            else:
                print("To add it to your PATH, run: \n")
                if os.path.exists(os.path.expanduser("~/.bashrc")):
                    print(f'echo "export PATH=\\{abs_binary_directory}:$PATH" >> ~/.bashrc')
                    print("source ~/.bashrc")
                elif os.path.exists(os.path.expanduser("~/.bash_profile")):
                    print(f'echo "export PATH=\\{abs_binary_directory}:$PATH" >> ~/.bash_profile')
                    print("source ~/.bash_profile")
                elif os.path.exists(os.path.expanduser("~/.zshrc")):
                    print(f'echo "export PATH=\\{abs_binary_directory}:$PATH" >> ~/.zshrc')
                    print("source ~/.zshrc")
                else:
                    print(f"export PATH={abs_binary_directory}:$PATH")


if __name__ == "__main__":
    main()
