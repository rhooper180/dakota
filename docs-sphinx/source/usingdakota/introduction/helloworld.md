Dakota Beginner's Tutorial
============

```{eval-rst}
.. _helloworld:
```

This section is intended for users who are new to Dakota, to demonstrate the basics of running a simple example.

## First Steps

1. Make sure Dakota runs. You should see Dakota version information when you type `dakota -v`.
2. Create a working directory.
3. Copy `rosen_multidim.in` from the `dakota/share/dakota/examples/users/` directory to your working directory.
4. From the working directory, run:

```
dakota -i rosen multidim.in -o rosen multidim.out > rosen multidim.stdout
```

## What should happen

Dakota outputs a large amount of information to help users track progress. Four files should have been created:
1. The **screen output** has been redirected to the file `rosen_multidim.stdout`. The contents are messages from
Dakota and notes about the progress of the iterator (i.e. method/algorithm).
2. The **output file** `rosen_multidim.out` contains information about the function evaluations.
3. `rosen_multidim.dat` is created due to the specification of a **tabular data file.** This summarizes the variables and responses for each function evaluation.
4. `dakota.rst` is a **restart file**. If a Dakota analysis is interrupted, it can be often be restarted without losing all progress.
Dakota has some data processing capabilities for output analysis. The output file will contain the relevant results. In this case,
the output file has details about each of the 81 function evaluations. For more advanced or customized data processing or
visualization, the tabular data file can be imported into another analysis tool.

## What now?
* Assuming Dakota ran successfully, [skim the three text files](../output/classicoutput.md) (restart files are in a binary format).
* [Learn more about this example's parameter study method, and the rosenbrock test problem.](../examples/gettingstarted.md)
* [Explore the many methods available in Dakota](../methodtypes.rst).
* [Learn the syntax needed to use these methods](../inputfile.md).
* [Learn how to use your own analysis code with Dakota](../advanced/advancedsimulationcodeinterfaces.md).

## Video Resources

[![alt text](img/DakotaSimpleExampleScreencastTeaser.png "Watch Screencast 1.1: Running a Simple Example in Dakota")](https://www.youtube.com/watch?v=ofi13UTq_Is&list=PLouetuxaIMDo-NMFXT-hlHYhOkePLrayY&index=1)