oomstaller : oomstaller.cpp
	c++ -Os -Wall oomstaller.cpp -o oomstaller

clean :
	rm -f oomstaller *~ *.s *.o *.out
