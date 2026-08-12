请在 Windows 环境中处理 BreezeDevTeam/CDDA-Breeze。
默认仓库地址：
https://github.com/BreezeDevTeam/CDDA-Breeze
开发时参照网址：
CCB分支：https://github.com/CrimsonCrossBunker/Cataclysm-Cleanwater-Bomb
游戏攻略手册，可以帮你初步了解游戏：https://surflurer.github.io/CDDA-CN-Guide/

一、工作环境

- 本地仓库：`D:\GitHub\CDDA-Breeze`
- GitHub 远程：`origin = BreezeDevTeam/CDDA-Breeze`
- PC 测试根目录：`D:\Games\CDDA`
- 固定覆盖源：`D:\Games\CDDA\breeze仓库`
- Windows 与 Android 测试工作流：`Windows & Android Test`
- GitHub 备份仓库：`https://github.com/AndyScarlet233/CDDA-Breeze-Andy`

二、工作流程

- 一般提交是在github从main建立新的分支，然后提交
- 有时候累加更新可能会需要提交到已有分支，这时候你需要注意不要提交错了，提交时做好代码审查
- PR 标题必须使用中文，避免使用仅英文标题。
- 引用外部上游 PR 时，必须同时写明上游仓库全名和完整链接；`CCB123Pr` 指 `CrimsonCrossBunker/Cataclysm-Cleanwater-Bomb#123`，不得误写或暗示为 `BreezeDevTeam/CDDA-Breeze#123`。
- 完成后用户如果明确提到启动MSVC，则在github上启动测试工作流程，测试中不必持续监听，启动后即可结束任务
- 当用户要求测试游戏时，将测试 PC 的游戏文件下载到 `D:\Games\CDDA`，解压后删除压缩包；使用中文命名游戏文件夹，并在文件夹名称中写上本次测试分支的中文名，方便识别测试内容；最后用 `D:\Games\CDDA\breeze仓库` 中的文件覆盖测试目录。
- 不确定的话可以向用户提出选项窗口，不要轻易自己做出重要决定
- 添加进游戏的玩家可见内容必须有中文汉化，游戏汉化位置在po
- PR合并后原有分支的内容如果都已经提交完毕的话，可以安全删除
