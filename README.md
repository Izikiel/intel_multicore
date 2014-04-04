intel_multicore
===============

The objective of this project was to determinate how much performance improvement would be obtained if we executed an application on a bare bones kernel, which would only execute the application, reducing the overhead produced in more complex systems.

The kernel project per-se is named DeliriOS_64bits, and it currently executes tests with the following algorithms:

* HeapSort MonoCore
* MergeSort DualCore synchronized by memory.
* MergeSort DualCore synchronized by interprocessor interrupts.

* FFT implemented with the Cooley-Tukey algorithm monocore.
* FFT implemented with the Cooley-Tukey algorithm dualcore synchronized by memory.
* FFT implemented with the Cooley-Tukey algorithm dualcore synchronized by interprocessor interrupts.

We tested this algorithms on several processors of different architectures, obtaining pretty interesting results, which are in the DeliriOS_64bits/informe/informe.pdf file (for now it's only in spanish).

This work was developed by me (Izikiel), Silvio Vileriño (svilerino) , and Juan Pablo Darago (jpdarago).

jpdarago implemented the multicore initialization.

svilerino built the 64 bits kernel core, enabled multicore using and adapting jpdarago's work and enabled the project to boot with grub 0.97 by designing a several stages boot.

Izikiel implemented the tests and adapted existing algorithms to the project, re-organized the project structure, and implemented synchronization protocols.

More information about the kernel design, algorithms implementation and synchronization protocols can be obtained by reading the DeliriOS_64bits/informe/informe.pdf file (only in spanish).
