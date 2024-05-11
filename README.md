# GPS Tracker product project helper

This project contains related firmware/source code of the GPS tracker project.

## Project structure

Describe the project's folder structure and obligations that developers must
follow. Members' will be asked to reformat/change their code/file location
if these requirements are not met.

### **boards** folder

This is the **boards** definitions folder used **specifically** for this
project only. 

Follow the Zephyr's project instruction of how to create a custom board.

### **apps** folder

The **apps** folder contains only the source code/firmware that will be targets
for future release. **DO NOT** put your sample code/test project inside this 
folder, this is not the place for it.

For instance, if we have the firmware for a board `board_1` related to this
project, we should have a folder named:

```
gps_tracker_board_1
```

in the app folder. This simplifies the release process as we have absolute
clarity of which folder contains the actual production source code.

### **libs** folder

The **libs** folder contains project's specific libraries. Any pieces of code
that need to be reused across samples/app projects should be put here. These
libraries if they are used by other products can be moved into an SDK later.

All level-0 configuration options exported by the **libs** folder must have
the **LIBS_** suffix.

For example:

```
CONFIG_LIBS_GPIOS
```

for a custom library of GPIOs used only by this product.

### **documents** folder

Has needed documents related to the project's hardware/firmware. Members should
frequently update the document list in this folder to allow others quick access
to the necessary documents.

### **samples** folder

**samples** folder contains sample code for anything you want to create a
sample for. Any new project for testing purpose should be placed in this
folder. 

One example use case for the projects inside this folder is: A project to
test the combination of `modem` functions with `ethernet` ports for `board_1`.
This sample project should have a good, self-explanatory name for it that
starts with the board's name.

```
board_1_modem_ethernet_sample
```

### **tests** folder

This folder only contains functional tests for the products. The project inside
this folder will test only a single function of the targeted device/product.

```
board_1_gpio
```

This project tests the GPIO function of the product.

The project inside this folder should use the test tools and functions provided
by Zephyr such as the **z_test** test suite.

## Other requirements

- Minimally, each project should at least have a README.md file listing the 
command to build the project and all build options provided by the project.
- Each decision related to how the product/project is going should be
documented in the adr.md file.
- **Format** your code properly before pushing to the repository.
- **DO NOT** force push to `main` branch.
- **ALWAYS** create a new branch for new features/test purposes.

### ADR

The `ADR.md` is the **Architecture Decision Record** belonging to this product.
This documents all the important decisions made by members that affect the 
product's goals and road map.

### How to commit

* Don't commit more than that you state in your commit message
* Commit with a brief commit headline and a detail commit body
* Always use lowercase text, UPPERCASE are reserved for important purposes.

## License

No license for now
