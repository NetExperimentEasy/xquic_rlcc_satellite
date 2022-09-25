# rlcc 

# rlcc使用redis数据库，客户端库为Hiredis

# install redis on ubuntu
```
sudo apt install redis-server
```

流的唯一标识作为key_state：状态作为其值


hiredis使用方法
https://www.cnblogs.com/52fhy/p/9196527.html
redis返回状态
https://www.jianshu.com/p/361b6dd7261f



# 基本流程
## 算法整个过程是阻塞的
部署：
rlcc周期采样流信息，携带流标识写入redis(lock=0->1)，拥塞算法读取(lock=1)，然后等待拥塞算法的运算结果，写入动作(lock=1->0)，从redis中读取预测的动作，执行动作
算法侧根据状态预测动作，返回写入redis动作时，要带上对应状态所在的流的唯一标识

训练：
可以并行训练，即同时跑多条流，同时对算法进行梯度更新