# GitHub Releases

After the Windows and Linux CI checks pass, open **Actions → Build and publish release → Run workflow**. Enter the full SHA of the commit on `main`. The workflow reads the version from that commit's `CMakeLists.txt`.

Leave **Publish** off to build and test the packages and download them as workflow artifacts. Turn it on to create the GitHub Release and publish the two packages with `SHA256SUMS`. The publishing job verifies the uploaded digests and the final tag.

To publish packages from a successful run, enter its run ID in **Build run** with **Publish** on. The workflow requires the original build to use a workflow revision now on `main` and verifies that both archives match the requested source commit.

The Linux job builds in a pinned CentOS 7.9 container and audits glibc 2.17 compatibility. The Windows job uses the pinned source SDK and Windows overlay provenance checks. Its notice bundle comes from the verified v0.1.1 archive. If a dependency input changes, review and refresh the bundle as part of that dependency update.
