# Getting SlideNX Reborn into a GitHub repository

The project already includes a GitHub Actions workflow at `.github/workflows/build.yml`. This conversation's GitHub integration cannot create a brand-new repository, so the first repository creation step must be done by the account owner.

1. On GitHub, choose **New repository** and call it `SlideNX-Reborn`. For the simplest first push, leave **Add a README**, `.gitignore`, and license unchecked; these are already in this project.
2. From a terminal in this project folder, run:

   ```sh
   git init
   git add .
   git commit -m "Add SlideNX Reborn source and cloud build"
   git branch -M main
   git remote add origin https://github.com/YOUR-USERNAME/SlideNX-Reborn.git
   git push -u origin main
   ```

   Replace `YOUR-USERNAME` with your GitHub username. GitHub may prompt you to authenticate using your configured Git credential manager.
3. Open the repository's **Actions** tab. The first push should start **SlideNX Reborn build**. Open the run and check **Summary → Artifacts**.
4. Download `SlideNX-Reborn-SD-package` if the build succeeds. If it fails, download `SlideNX-Reborn-build-diagnostics` and inspect `build.log`.

Do not paste a personal access token into chat. The project is derived from SlideNX under GPL-2.0; the source and license are included.
