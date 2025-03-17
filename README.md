[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/DTppDqsR)
# CSCI350P2

usc email 1: mlee9568@usc.edu

usc email 2: jaehcho@usc.edu

How to compile:

On your local machine, begin by starting up the csci350 environment

Make sure to have Docker created and ch environment steps can be found here: https://github.com/camerondurham/cs350-docker
Once the ch environment is started, clone the correct Github repo inside of the Docker container

The directory should look like this /xv6_docker/cs350-docker/project-1-jaehcho_mlee9568/xv6-public-master
Once inside xv6-public-master, run the commands 'make clean && make' and follow that by running 'make qemu-nox'

How to run:

After following the compilation steps, 'make qemu-nox' will begin the program.
Run part 1 code by typing in 'threadtest1','threadtest2', 'threadtest3' . The expected output should be shown in the terminal.
Run part 2 code by typing in 'mutextest1','mutextest2' . The expected output should be shown in the terminal.