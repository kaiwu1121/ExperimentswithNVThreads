for i in {1..10}
do
echo "running........$i"
./ll_test_evalcg.o
sleep 1
./ll_test_evalcg.o
sleep 2
done
