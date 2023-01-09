#include "utility.h"
#include "lio_sam/cloud_info.h"

struct VelodynePointXYZIRT
{
    PCL_ADD_POINT4D
    PCL_ADD_INTENSITY;
    uint16_t ring;
    float time;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT (VelodynePointXYZIRT,
    (float, x, x) (float, y, y) (float, z, z) (float, intensity, intensity)
    (uint16_t, ring, ring) (float, time, time)
)

struct OusterPointXYZIRT {
    PCL_ADD_POINT4D;
    float intensity;
    uint32_t t;
    uint16_t reflectivity;  //������
    uint8_t ring;
    uint16_t noise;
    uint32_t range;
    //���ϴ˾䣬���ڲ���eigen��Լ���ķ�ʽ�����˸����new delete���ڴ���亯��
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT(OusterPointXYZIRT,
    (float, x, x) (float, y, y) (float, z, z) (float, intensity, intensity)
    (uint32_t, t, t) (uint16_t, reflectivity, reflectivity)
    (uint8_t, ring, ring) (uint16_t, noise, noise) (uint32_t, range, range)
)

// Use the Velodyne point format as a common representation
using PointXYZIRT = VelodynePointXYZIRT;

const int queueLength = 2000;

class ImageProjection : public ParamServer
{
private:

    std::mutex imuLock;
    std::mutex odoLock;

    ros::Subscriber subLaserCloud;
    ros::Publisher  pubLaserCloud;
    
    ros::Publisher pubExtractedCloud;
    ros::Publisher pubLaserCloudInfo;

    ros::Subscriber subImu;
    std::deque<sensor_msgs::Imu> imuQueue;

    ros::Subscriber subOdom;
    std::deque<nav_msgs::Odometry> odomQueue;

    std::deque<sensor_msgs::PointCloud2> cloudQueue;
    sensor_msgs::PointCloud2 currentCloudMsg;

    double *imuTime = new double[queueLength];
    double *imuRotX = new double[queueLength];
    double *imuRotY = new double[queueLength];
    double *imuRotZ = new double[queueLength];

    int imuPointerCur;
    bool firstPointFlag;
    Eigen::Affine3f transStartInverse;

    pcl::PointCloud<PointXYZIRT>::Ptr laserCloudIn;
    pcl::PointCloud<OusterPointXYZIRT>::Ptr tmpOusterCloudIn;
    pcl::PointCloud<PointType>::Ptr   fullCloud;
    pcl::PointCloud<PointType>::Ptr   extractedCloud;

    int deskewFlag;
    cv::Mat rangeMat;

    bool odomDeskewFlag;
    float odomIncreX;
    float odomIncreY;
    float odomIncreZ;

    lio_sam::cloud_info cloudInfo;
    double timeScanCur;
    double timeScanEnd;
    std_msgs::Header cloudHeader;

    vector<int> columnIdnCountVec;


public:
    ImageProjection():
    deskewFlag(0)
    {
         //���Ļ������ص����� imu����   �������.
         // imuTopic:topic name; 2000:queue size; &ImageProjection::imuHandler:callback function
         // this: �������class��ķ��غ���������ʹ�õ��ĸ������������и������listener��
         // ���øö����ڲ��Ļص�����������&listener����������Լ��ģ�����thisָ��
         // ros::TransportHints().tcpNoDelay() :������ָ��hints��ȷ�����������û���ķ�ʽ:���ӳٵ�TCP���䷽ʽ
        subImu        = nh.subscribe<sensor_msgs::Imu>(imuTopic, 2000, &ImageProjection::imuHandler, this, ros::TransportHints().tcpNoDelay());
        //���Ļ������ص�����  
        //����imu��̼�: ����IMUPreintegration(IMUPreintegration.cpp�е���IMUPreintegration)��������̼ƻ��⣨����ʽ��
        subOdom       = nh.subscribe<nav_msgs::Odometry>(odomTopic+"_incremental", 2000, &ImageProjection::odometryHandler, this, ros::TransportHints().tcpNoDelay());
        //���Ļ������ص�����   �������
        subLaserCloud = nh.subscribe<sensor_msgs::PointCloud2>(pointCloudTopic, 5, &ImageProjection::cloudHandler, this, ros::TransportHints().tcpNoDelay());

        //����ȥ����ĵ���,"lio_sam/deskew/cloud_deskewed":topic name; 1:queue_size
        pubExtractedCloud = nh.advertise<sensor_msgs::PointCloud2> ("lio_sam/deskew/cloud_deskewed", 1);
        //�������������Ϣ ���ｨ�鿴һ���Զ����lio_sam::cloud_info��msg�ļ� ��������˽϶���Ϣ
        pubLaserCloudInfo = nh.advertise<lio_sam::cloud_info> ("lio_sam/deskew/cloud_info", 1);

        allocateMemory();
        resetParameters();

        pcl::console::setVerbosityLevel(pcl::console::L_ERROR);
    }

    void allocateMemory()
    {
        //����params.yaml�и�����N_SCAN Horizon_SCAN����ֵ�����ڴ�
        //������ָ���reset�����ڹ��캯��������г�ʼ��
        laserCloudIn.reset(new pcl::PointCloud<PointXYZIRT>());
        tmpOusterCloudIn.reset(new pcl::PointCloud<OusterPointXYZIRT>());
        fullCloud.reset(new pcl::PointCloud<PointType>());
        extractedCloud.reset(new pcl::PointCloud<PointType>());

        fullCloud->points.resize(N_SCAN*Horizon_SCAN);

        //cloudinfo��msg�ļ����Զ����cloud_info��Ϣ�������еı������и�ֵ����
        //(int size, int value):size-Ҫ�����ֵ��,value-Ҫ������������Ƶ�ֵ
        cloudInfo.startRingIndex.assign(N_SCAN, 0);
        cloudInfo.endRingIndex.assign(N_SCAN, 0);

        cloudInfo.pointColInd.assign(N_SCAN*Horizon_SCAN, 0);
        cloudInfo.pointRange.assign(N_SCAN*Horizon_SCAN, 0);

        resetParameters();
    }

    void resetParameters()
    {
        //�������
        laserCloudIn->clear();
        extractedCloud->clear();
        // reset range matrix for range image projection
        //��ʼȫ����FLT_MAX ��䣬
        //��˺��ĺ���projectPointCloud����һ��if (rangeMat.at<float>(rowIdn, columnIdn) != FLT_MAX) continue;
        rangeMat = cv::Mat(N_SCAN, Horizon_SCAN, CV_32F, cv::Scalar::all(FLT_MAX));

        imuPointerCur = 0;
        firstPointFlag = true;
        odomDeskewFlag = false;

        for (int i = 0; i < queueLength; ++i)
        {
            imuTime[i] = 0;
            imuRotX[i] = 0;
            imuRotY[i] = 0;
            imuRotZ[i] = 0;
        }

        columnIdnCountVec.assign(N_SCAN, 0);
    }

    ~ImageProjection(){}

    /**
     * ����ԭʼimu����
     * 1��imuԭʼ��������ת����lidarϵ�����ٶȡ����ٶȡ�RPY
    */
    void imuHandler(const sensor_msgs::Imu::ConstPtr& imuMsg)
    {
        //imuConverter��ͷ�ļ�utility.h�У������ǰ�imu����ת����lidar����ϵ
        sensor_msgs::Imu thisImu = imuConverter(*imuMsg);

        // ������������ݵ�ʱ����в�����
        std::lock_guard<std::mutex> lock1(imuLock);
        imuQueue.push_back(thisImu);

        // debug IMU data
        // cout << std::setprecision(6);
        // cout << "IMU acc: " << endl;
        // cout << "x: " << thisImu.linear_acceleration.x << 
        //       ", y: " << thisImu.linear_acceleration.y << 
        //       ", z: " << thisImu.linear_acceleration.z << endl;
        // cout << "IMU gyro: " << endl;
        // cout << "x: " << thisImu.angular_velocity.x << 
        //       ", y: " << thisImu.angular_velocity.y << 
        //       ", z: " << thisImu.angular_velocity.z << endl;
        // double imuRoll, imuPitch, imuYaw;
        // tf::Quaternion orientation;
        // tf::quaternionMsgToTF(thisImu.orientation, orientation);
        // tf::Matrix3x3(orientation).getRPY(imuRoll, imuPitch, imuYaw);
        // cout << "IMU roll pitch yaw: " << endl;
        // cout << "roll: " << imuRoll << ", pitch: " << imuPitch << ", yaw: " << imuYaw << endl << endl;
    }

    /**
     * ����imu��̼ƣ���imuPreintegration���ּ���õ���ÿʱ��imuλ��.(��ͼ�Ż������з�����)
    */
    void odometryHandler(const nav_msgs::Odometry::ConstPtr& odometryMsg)
    {
        std::lock_guard<std::mutex> lock2(odoLock);
        odomQueue.push_back(*odometryMsg);
    }

    void cloudHandler(const sensor_msgs::PointCloud2ConstPtr& laserCloudMsg)
    {
        //���һ֡������Ƶ����У�ȡ������һ֡��Ϊ��ǰ֡��������ֹʱ��������������Ч��
        if (!cachePointCloud(laserCloudMsg))
            return;

        // ��ǰ֡��ֹʱ�̶�Ӧ��imu���ݡ�imu��̼����ݴ���
        if (!deskewInfo())
            return;

        // ��ǰ֡��������˶�����У��
        // 1����鼤�����롢ɨ�����Ƿ�Ϲ�
        // 2�������˶�����У�������漤���
        projectPointCloud();

        // ��ȡ��Ч����㣬����extractedCloud
        cloudExtraction();

        // ������ǰ֡У������ƣ���Ч��
        publishClouds();

        // ���ò���������ÿ֡lidar���ݶ�Ҫ������Щ����
        resetParameters();
    }

    bool cachePointCloud(const sensor_msgs::PointCloud2ConstPtr& laserCloudMsg)
    {
        // cache point cloud
        cloudQueue.push_back(*laserCloudMsg);
        if (cloudQueue.size() <= 2)
            return false;

        // convert cloud
        // ȡ��������ƶ����������һ֡
        currentCloudMsg = std::move(cloudQueue.front());
        cloudQueue.pop_front();
        if (sensor == SensorType::VELODYNE || sensor == SensorType::LIVOX)
        {
            // ת����pcl���Ƹ�ʽ �β�: (in,out)
            pcl::moveFromROSMsg(currentCloudMsg, *laserCloudIn);
        }
        else if (sensor == SensorType::OUSTER)
        {
            // Convert to Velodyne format
            pcl::moveFromROSMsg(currentCloudMsg, *tmpOusterCloudIn);
            laserCloudIn->points.resize(tmpOusterCloudIn->size());
            laserCloudIn->is_dense = tmpOusterCloudIn->is_dense;
            for (size_t i = 0; i < tmpOusterCloudIn->size(); i++)
            {
                auto &src = tmpOusterCloudIn->points[i];
                auto &dst = laserCloudIn->points[i];
                dst.x = src.x;
                dst.y = src.y;
                dst.z = src.z;
                dst.intensity = src.intensity;
                dst.ring = src.ring;
                dst.time = src.t * 1e-9f;
            }
        }
        else
        {
            ROS_ERROR_STREAM("Unknown sensor type: " << int(sensor));
            ros::shutdown();
        }

        // get timestamp
        cloudHeader = currentCloudMsg.header;
        //��һ���ʱ�䱻��¼����������timeScanCur�У�����deskewPoint�лᱻ����laserCloudIn->points[i].time
        timeScanCur = cloudHeader.stamp.toSec();
        //���Կ���lasercloudin�д洢��time��һ֡�о�����ʼ������ʱ��,timeScanEnd�Ǹ�֡���ƵĽ�βʱ��
        timeScanEnd = timeScanCur + laserCloudIn->points.back().time;

        // check dense flag
        if (laserCloudIn->is_dense == false)
        {
            ROS_ERROR("Point cloud is not in dense format, please remove NaN points first!");
            ros::shutdown();
        }

        // check ring channel
        //����static�ؼ��֣�ֻ���һ�Σ����ring���field�Ƿ����. veloodyne��ouster����;
        //ring����������0������������
        static int ringFlag = 0;
        if (ringFlag == 0)
        {
            ringFlag = -1;
            for (int i = 0; i < (int)currentCloudMsg.fields.size(); ++i)
            {
                if (currentCloudMsg.fields[i].name == "ring")
                {
                    ringFlag = 1;
                    break;
                }
            }
            if (ringFlag == -1)
            {
                ROS_ERROR("Point cloud ring channel not available, please configure your point cloud data!");
                ros::shutdown();
            }
        }

        // check point time
        // ����Ƿ����timeͨ��
        if (deskewFlag == 0)
        {
            deskewFlag = -1;
            for (auto &field : currentCloudMsg.fields)
            {
                if (field.name == "time" || field.name == "t")
                {
                    deskewFlag = 1;
                    break;
                }
            }
            if (deskewFlag == -1)
                ROS_WARN("Point cloud timestamp not available, deskew function disabled, system will drift significantly!");
        }

        return true;
    }

    bool deskewInfo()
    {
        std::lock_guard<std::mutex> lock1(imuLock);
        std::lock_guard<std::mutex> lock2(odoLock);

        // make sure IMU data available for the scan
        // Ҫ��imu����ʱ���ϰ����������ݣ��������´�����
        if (imuQueue.empty() || imuQueue.front().header.stamp.toSec() > timeScanCur || imuQueue.back().header.stamp.toSec() < timeScanEnd)
        {
            ROS_DEBUG("Waiting for IMU data ...");
            return false;
        }

        // ��ǰ֡��Ӧimu���ݴ���
        // 1��������ǰ����֡��ֹʱ��֮���imu���ݣ���ʼʱ�̶�Ӧimu����̬��RPY��Ϊ��ǰ֡�ĳ�ʼ��̬��
        // 2���ý��ٶȡ�ʱ����֣�����ÿһʱ������ڳ�ʼʱ�̵���ת������ʼʱ����ת��Ϊ0
        // ע��imu���ݶ��Ѿ�ת����lidarϵ����
        //imuȥ����������� 
        imuDeskewInfo();

        // ��ǰ֡��Ӧimu��̼ƴ���
        // 1��������ǰ����֡��ֹʱ��֮���imu��̼����ݣ���ʼʱ�̶�Ӧimu��̼���Ϊ��ǰ֡�ĳ�ʼλ��
        // 2������ʼ����ֹʱ�̶�Ӧimu��̼ƣ��������λ�˱任������ƽ������
        // ע��imu���ݶ��Ѿ�ת����lidarϵ����
        //��̼�ȥ�����������
        odomDeskewInfo();

        return true;
    }

    void imuDeskewInfo()
    {
        cloudInfo.imuAvailable = false;

        // ��imu������ɾ����ǰ����֡0.01sǰ��ʱ�̵�imu����
        while (!imuQueue.empty())
        {
            if (imuQueue.front().header.stamp.toSec() < timeScanCur - 0.01)
                imuQueue.pop_front();
            else
                break;
        }

        if (imuQueue.empty())
            return;

        imuPointerCur = 0;

        // ������ǰ����֡��ֹʱ�̣�ǰ����չ0.01s��֮���imu����
        for (int i = 0; i < (int)imuQueue.size(); ++i)
        {
            sensor_msgs::Imu thisImuMsg = imuQueue[i];
            double currentImuTime = thisImuMsg.header.stamp.toSec();

            // get roll, pitch, and yaw estimation for this scan
            // ��ȡimu��̬��RPY����Ϊ��ǰlidar֡��ʼ��̬��
            if (currentImuTime <= timeScanCur)
                imuRPY2rosRPY(&thisImuMsg, &cloudInfo.imuRollInit, &cloudInfo.imuPitchInit, &cloudInfo.imuYawInit);

            // ������ǰ����֡����ʱ��0.01s������
            if (currentImuTime > timeScanEnd + 0.01)
                break;

            // ��һ֡imu��ת�ǳ�ʼ��
            if (imuPointerCur == 0){
                imuRotX[0] = 0;
                imuRotY[0] = 0;
                imuRotZ[0] = 0;
                imuTime[0] = currentImuTime;
                ++imuPointerCur;
                continue;
            }

            // get angular velocity
            // ��ȡimu���ٶ�
            double angular_x, angular_y, angular_z;
            imuAngular2rosAngular(&thisImuMsg, &angular_x, &angular_y, &angular_z);

            // integrate rotation
            double timeDiff = currentImuTime - imuTime[imuPointerCur-1];
            imuRotX[imuPointerCur] = imuRotX[imuPointerCur-1] + angular_x * timeDiff;
            imuRotY[imuPointerCur] = imuRotY[imuPointerCur-1] + angular_y * timeDiff;
            imuRotZ[imuPointerCur] = imuRotZ[imuPointerCur-1] + angular_z * timeDiff;
            imuTime[imuPointerCur] = currentImuTime;
            ++imuPointerCur;
        }

        --imuPointerCur;
        // û�кϹ��imu����
        if (imuPointerCur <= 0)
            return;

        cloudInfo.imuAvailable = true;
    }

    //��ʼpose��Ϣ������cloudInfo��
    void odomDeskewInfo()
    {
        cloudInfo.odomAvailable = false;
        // ��imu��̼ƶ�����ɾ����ǰ����֡0.01sǰ��ʱ�̵�imu����
        while (!odomQueue.empty())
        {
            if (odomQueue.front().header.stamp.toSec() < timeScanCur - 0.01)
                odomQueue.pop_front();
            else
                break;
        }

        if (odomQueue.empty())
            return;

        // Ҫ������е�ǰ����֡ʱ��֮ǰ����̼�����
        if (odomQueue.front().header.stamp.toSec() > timeScanCur)
            return;

        // get start odometry at the beinning of the scan(��ͼ�Ż������з�����)
        nav_msgs::Odometry startOdomMsg;

        for (int i = 0; i < (int)odomQueue.size(); ++i)
        {
            startOdomMsg = odomQueue[i];
            // ��cloudHandler��cachePointCloud�����У�timeScanCur = cloudHeader.stamp.toSec();������ǰ֡���Ƶĳ�ʼʱ��
             //�ҵ���һ�����ڳ�ʼʱ�̵�odom
            if (ROS_TIME(&startOdomMsg) < timeScanCur)
                continue;
            else
                break;
        }

        // ��ȡimu��̼���̬��
        tf::Quaternion orientation;
        tf::quaternionMsgToTF(startOdomMsg.pose.pose.orientation, orientation);

        double roll, pitch, yaw;
        tf::Matrix3x3(orientation).getRPY(roll, pitch, yaw);

        // Initial guess used in mapOptimization
        // �õ�ǰ����֡��ʼʱ�̵�imu��̼ƣ���ʼ��lidarλ�ˣ���������mapOptmization
        cloudInfo.initialGuessX = startOdomMsg.pose.pose.position.x;
        cloudInfo.initialGuessY = startOdomMsg.pose.pose.position.y;
        cloudInfo.initialGuessZ = startOdomMsg.pose.pose.position.z;
        cloudInfo.initialGuessRoll  = roll;
        cloudInfo.initialGuessPitch = pitch;
        cloudInfo.initialGuessYaw   = yaw;

        cloudInfo.odomAvailable = true;

        // get end odometry at the end of the scan
        odomDeskewFlag = false;
        // �����ǰ����֡����ʱ��֮��û��imu��̼����ݣ�����
        if (odomQueue.back().header.stamp.toSec() < timeScanEnd)
            return;

        nav_msgs::Odometry endOdomMsg;
        // ��ȡ��ǰ����֡����ʱ�̵�imu��̼�
        for (int i = 0; i < (int)odomQueue.size(); ++i)
        {
            endOdomMsg = odomQueue[i];
            // ��cloudHandler��cachePointCloud�����У�       timeScanEnd = timeScanCur + laserCloudIn->points.back().time;
            // �ҵ���һ������һ֡�������ʱ�̵�odom
            if (ROS_TIME(&endOdomMsg) < timeScanEnd)
                continue;
            else
                break;
        }

        // �����ֹʱ�̶�Ӧimu��̼Ƶķ���ȣ�����
        if (int(round(startOdomMsg.pose.covariance[0])) != int(round(endOdomMsg.pose.covariance[0])))
            return;
        //�о�֮��������Ϣ��û�б��õ�
        Eigen::Affine3f transBegin = pcl::getTransformation(startOdomMsg.pose.pose.position.x, startOdomMsg.pose.pose.position.y, startOdomMsg.pose.pose.position.z, roll, pitch, yaw);

        tf::quaternionMsgToTF(endOdomMsg.pose.pose.orientation, orientation);
        tf::Matrix3x3(orientation).getRPY(roll, pitch, yaw);
        Eigen::Affine3f transEnd = pcl::getTransformation(endOdomMsg.pose.pose.position.x, endOdomMsg.pose.pose.position.y, endOdomMsg.pose.pose.position.z, roll, pitch, yaw);
        // ��ֹʱ��imu��̼Ƶ���Ա任
        Eigen::Affine3f transBt = transBegin.inverse() * transEnd;
        // ��Ա任����ȡ����ƽ�ơ���ת��ŷ���ǣ�
        float rollIncre, pitchIncre, yawIncre;
        // ������ת���У���ȡXYZ�Լ�ŷ����,ͨ��tranBt �������ֵ  ����ȥ�����õ�
        pcl::getTranslationAndEulerAngles(transBt, odomIncreX, odomIncreY, odomIncreZ, rollIncre, pitchIncre, yawIncre);

        odomDeskewFlag = true;
    }

    /**
     * �ڵ�ǰ����֡��ֹʱ�䷶Χ�ڣ�����ĳһʱ�̵���ת���������ʼʱ�̵���ת������
    */
    void findRotation(double pointTime, float *rotXCur, float *rotYCur, float *rotZCur)
    {
        *rotXCur = 0; *rotYCur = 0; *rotZCur = 0;

        // ���ҵ�ǰʱ����imuTime�µ�����
        int imuPointerFront = 0;
        //imuDeskewInfo�У���imuPointerCur���м���(������������ǰ����֡����ʱ��0.01s)
        while (imuPointerFront < imuPointerCur)
        {
            //imuTime��imuDeskewInfo��deskewInfo�е��ã�deskewInfo��cloudHandler�е��ã�����ֵ����imuQueue��ȡֵ
            //pointTimeΪ��ǰʱ�̣��ɴ˺����ĺ����βδ���,Ҫ�ҵ�imu�����б����һ�����ڵ�ǰʱ�������
            if (pointTime < imuTime[imuPointerFront])
                break;
            ++imuPointerFront;
        }

        // ��Ϊ�뵱ǰʱ���������ת����
        //�������Ϊ0��ô�imuʱ���С���˵�ǰʱ���(�쳣�˳�)
        if (pointTime > imuTime[imuPointerFront] || imuPointerFront == 0)
        {
            //δ�ҵ����ڵ�ǰʱ�̵�imu��������
            //imuRotX��Ϊ֮ǰ���ֳ�������.(imuDeskewInfo��)
            *rotXCur = imuRotX[imuPointerFront];
            *rotYCur = imuRotY[imuPointerFront];
            *rotZCur = imuRotZ[imuPointerFront];
        } else {
            // ǰ��ʱ�̲�ֵ���㵱ǰʱ�̵���ת����
            //��ʱfront��ʱ���Ǵ��ڵ�ǰpointTimeʱ�䣬back=front-1�պ�С�ڵ�ǰpointTimeʱ�䣬ǰ��ʱ�̲�ֵ����
            int imuPointerBack = imuPointerFront - 1;
            //��һ�¸õ�ʱ����ڱ���imu�е�λ��
            double ratioFront = (pointTime - imuTime[imuPointerBack]) / (imuTime[imuPointerFront] - imuTime[imuPointerBack]);
            double ratioBack = (imuTime[imuPointerFront] - pointTime) / (imuTime[imuPointerFront] - imuTime[imuPointerBack]);
            //��������Ϊ��������ֵ�����β�ָ��ķ�ʽ����
            //��ǰ��ٷֱȸ�����ת��
            *rotXCur = imuRotX[imuPointerFront] * ratioFront + imuRotX[imuPointerBack] * ratioBack;
            *rotYCur = imuRotY[imuPointerFront] * ratioFront + imuRotY[imuPointerBack] * ratioBack;
            *rotZCur = imuRotZ[imuPointerFront] * ratioFront + imuRotZ[imuPointerBack] * ratioBack;
        }
    }

    void findPosition(double relTime, float *posXCur, float *posYCur, float *posZCur)
    {
        // ����������ƶ��ٶȽ��������������ߵ��ٶȣ���ô������Ϊ������һ֡ʱ�䷶Χ�ڣ�ƽ����С�����Ժ��Բ���
        *posXCur = 0; *posYCur = 0; *posZCur = 0;

        // If the sensor moves relatively slow, like walking speed, positional deskew seems to have little benefits. Thus code below is commented.

        // if (cloudInfo.odomAvailable == false || odomDeskewFlag == false)
        //     return;

        // float ratio = relTime / (timeScanEnd - timeScanCur);

        // *posXCur = ratio * odomIncreX;
        // *posYCur = ratio * odomIncreY;
        // *posZCur = ratio * odomIncreZ;
    }

    /**
     * �����˶�����У��
     * ���õ�ǰ֡��ֹʱ��֮���imu���ݼ�����ת������imu��̼����ݼ���ƽ��������������ÿһʱ�̼����λ�ñ任����һ�����������ϵ�£������˶�����
    */
    PointType deskewPoint(PointType *point, double relTime)
    {
        //�����Դ�����ĵ�ʱ���ͨ����imu�����жϣ�û�л��ǲ������򷵻ص�
        if (deskewFlag == -1 || cloudInfo.imuAvailable == false)
            return *point;

        //���ʱ�����scanʱ���relTime�����ĵ�laserCloudIn->points[i].time��
        //lasercloudin�д洢��time��һ֡�о�����ʼ������ʱ��
        // ��cloudHandler��cachePointCloud�����У�timeScanCur = cloudHeader.stamp.toSec();������ǰ֡���Ƶĳ�ʼʱ��
        //������Ӽ��ɵõ���ǰ���׼ȷʱ��
        double pointTime = timeScanCur + relTime;

        //����ʱ�����ֵ��ȡimu�������ת����λ������ע��imu������������ʼʱ�̵���ת������
        float rotXCur, rotYCur, rotZCur;
        findRotation(pointTime, &rotXCur, &rotYCur, &rotZCur);

        float posXCur, posYCur, posZCur;
        findPosition(relTime, &posXCur, &posYCur, &posZCur);

        //�����firstPointFlag��Դ��resetParameters��������resetParameters����ÿ��ros����cloudHandler��������
        //��һ�����λ��������0��������
        if (firstPointFlag == true)
        {
            transStartInverse = (pcl::getTransformation(posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur)).inverse();
            //�ĳ�false�Ժ�ͬһ֡�������ݵ���һ�ξͲ�ִ����
            firstPointFlag = false;
        }

        // transform points to start
        //ɨ�赱ǰ��ʱlidar����������ϵ�±任����
        Eigen::Affine3f transFinal = pcl::getTransformation(posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur);
        //ɨ��õ����ɨ�豾��scan��һ�����lidar�任����=
        //��һ����ʱlidar��������ϵ�±任����������ǰ��ʱlidar��������ϵ�±任����
        //Tij=Twi^-1 * Twj
        //ע������׼ȷ����˵��������������ϵ,
        //���ݴ����������ǰ�imu����:
        //��imuDeskewInfo�����У��ڵ�ǰ����֡��ʼ��ǰ0.01���imu���ݿ�ʼ���֣�
        //������Ϊԭ�㣬Ȼ���ȡ��ǰ����֡��һ����ʱ�̵�λ������transStartInverse��
        //�͵�ǰ��ʱ�̵�λ������transFinal����������������߱任transBt��
        //�����ԵĲ��ǡ���������ϵ����
        //���ǡ���ǰ����֡��ʼǰ��0.01����״�����ϵ����imucallback�������Ѿ���imuת�������״�����ϵ�ˣ�
        Eigen::Affine3f transBt = transStartInverse * transFinal;

        PointType newPoint;
        //����lidarλ�˱任 Tij����������λ��: Tij * Pj
        newPoint.x = transBt(0,0) * point->x + transBt(0,1) * point->y + transBt(0,2) * point->z + transBt(0,3);
        newPoint.y = transBt(1,0) * point->x + transBt(1,1) * point->y + transBt(1,2) * point->z + transBt(1,3);
        newPoint.z = transBt(2,0) * point->x + transBt(2,1) * point->y + transBt(2,2) * point->z + transBt(2,3);
        newPoint.intensity = point->intensity;

        return newPoint;
    }

    void projectPointCloud()
    {
        //���������� ��������һ������ͶӰ
        int cloudSize = laserCloudIn->points.size();
        // range image projection
        for (int i = 0; i < cloudSize; ++i)
        {
            PointType thisPoint;
            //laserCloudIn����ԭʼ�ĵ��ƻ����е�����
            thisPoint.x = laserCloudIn->points[i].x;
            thisPoint.y = laserCloudIn->points[i].y;
            thisPoint.z = laserCloudIn->points[i].z;
            thisPoint.intensity = laserCloudIn->points[i].intensity;

            float range = pointDistance(thisPoint);
            if (range < lidarMinRange || range > lidarMaxRange)
                continue;

            //����ͼ�����  �������ring��Ӧ,
            //rowIdn������õ㼤���״���ˮƽ�����ϵڼ��ߵġ��������ϼ�����-15�ȼ�Ϊ��ʼ�ߣ���0�ߣ�һ��16��(N_SCAN=16
            int rowIdn = laserCloudIn->points[i].ring;
            if (rowIdn < 0 || rowIdn >= N_SCAN)
                continue;

            if (rowIdn % downsampleRate != 0)
                continue;

            int columnIdn = -1;
            if (sensor == SensorType::VELODYNE || sensor == SensorType::OUSTER)
            {
                //ˮƽ�Ƿֱ���
                float horizonAngle = atan2(thisPoint.x, thisPoint.y) * 180 / M_PI;
                //Horizon_SCAN=1800,ÿ��0.2��
                static float ang_res_x = 360.0/float(Horizon_SCAN);
                //horizonAngle Ϊ[-180,180],horizonAngle -90 Ϊ[-270,90],-round Ϊ[-90,270], /ang_res_x Ϊ[-450,1350]
                //+Horizon_SCAN/2Ϊ[450,2250]
               // ����horizonAngle��[-180,180]ӳ�䵽[450,2250]
                columnIdn = -round((horizonAngle-90.0)/ang_res_x) + Horizon_SCAN/2;
               //����1800�����ȥ1800���൱�ڰ�1801��2250ӳ�䵽1��450
              //�Ȱ�columnIdn��horizonAngle:(-PI,PI]ת����columnIdn:[H/4,5H/4],
              //Ȼ���ж�columnIdn��С����H��5H/4�Ĳ���������������0��H/4�Ĳ��֡�
              //�����ķ�Χת������[0,H] (H:Horizon_SCAN)��
              //�����Ͱ�ɨ�迪ʼ�ĵط��Ƕ�Ϊ0��Ƕ�Ϊ360��������һ�𣬷ǳ����
              //���ǰ����x�������y����ô���������180���ұ���-180������Ĳ������ǣ�����չ����һ��ͼ:
              //                   0
              //   90                        -90
              //          180 || (-180)
              //  (-180)   -----   (-90)  ------  0  ------ 90 -------180
              //��Ϊ:  90 ----180(-180) ---- (-90)  ----- (0)    ----- 90
                if (columnIdn >= Horizon_SCAN)
                    columnIdn -= Horizon_SCAN;
            }
            else if (sensor == SensorType::LIVOX)
            {
                columnIdn = columnIdnCountVec[rowIdn];
                columnIdnCountVec[rowIdn] += 1;
            }
            
            if (columnIdn < 0 || columnIdn >= Horizon_SCAN)
                continue;

            if (rangeMat.at<float>(rowIdn, columnIdn) != FLT_MAX)
                continue;

            //ȥ����  �˶����� ������Ҫ�õ��״���Ϣ�е�time ���field
            thisPoint = deskewPoint(&thisPoint, laserCloudIn->points[i].time);
            //ͼ��������ŷ��������
            rangeMat.at<float>(rowIdn, columnIdn) = range;

            // ת����һά�������洢У��֮��ļ����
            int index = columnIdn + rowIdn * Horizon_SCAN;
            fullCloud->points[index] = thisPoint;
        }
    }

    void cloudExtraction()
    {
        // ��Ч���������
        int count = 0;
        // extract segmented cloud for lidar odometry
        for (int i = 0; i < N_SCAN; ++i)
        {
            //��ȡ������ʱ��ÿһ�е�ǰ5�������5��������
            //��¼ÿ��ɨ������ʼ��5���������һά�����е�����
            //cloudInfoΪ�Զ����msg
            // ��¼ÿ��ɨ������ʼ��5���������һά�����е�����
            cloudInfo.startRingIndex[i] = count - 1 + 5;

            for (int j = 0; j < Horizon_SCAN; ++j)
            {
                if (rangeMat.at<float>(i,j) != FLT_MAX)
                {
                    // mark the points' column index for marking occlusion later
                    // ��¼������Ӧ��Horizon_SCAN�����ϵ�����
                    cloudInfo.pointColInd[count] = j;
                    // save range info ��������
                    cloudInfo.pointRange[count] = rangeMat.at<float>(i,j);
                    // save extracted cloud
                    // ������Ч�����
                    extractedCloud->push_back(fullCloud->points[j + i*Horizon_SCAN]);
                    // size of extracted cloud
                    ++count;
                }
            }
            // ��¼ÿ��ɨ���ߵ�����5���������һά�����е�����
            cloudInfo.endRingIndex[i] = count -1 - 5;
        }
    }
    
    /**
     * ������ǰ֡У������ƣ���Ч��
    */
    void publishClouds()
    {
        cloudInfo.header = cloudHeader;
        //publishCloud��utility.hͷ�ļ���,��Ҫ���뷢�����pubExtractedCloud����ȡ������Ч���ƣ���֡ʱ�����
        //pubExtractedCloud�����ڹ��캯���У���������ȥ����ĵ���.
        //extractedCloud��Ҫ��cloudExtraction�б���ȡ�����Ʊ�ȥ���˻��䣬
        //����ÿ��ͷ����ͺ������Ҫ(����Ȼ�����棬����֮������ȡ����ʱ��Ҫ,��ΪҪ����ǰ������������ʣ�
        //cloudHeader.stamp ��Դ��currentCloudMsg,cloudHeader��cachePointCloud�б���ֵcurrentCloudMsg.header
        //��currentCloudMsg�ǵ��ƶ���cloudQueue����ȡ��
        //lidarFrame:��utility.h�б���Ϊbase_link,
        //��publishCloud�����У�tempCloud.header.frame_id="base_link"(lidarFrame)
        //֮���÷������pubExtractedCloud������ȥ����ĵ���
        cloudInfo.cloud_deskewed  = publishCloud(pubExtractedCloud, extractedCloud, cloudHeader.stamp, lidarFrame);
        //�����Զ���cloud_info��Ϣ
        pubLaserCloudInfo.publish(cloudInfo);

        //pubExtractedCloud������ֻ�е�����Ϣ����pubLaserCloudInfo������Ϊ�Զ���ĺܶ���Ϣ
    }
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "lio_sam");

    ImageProjection IP;
    
    ROS_INFO("\033[1;32m----> Image Projection Started.\033[0m");
    //����һЩֻ����һ������ļ򵥽ڵ���˵������ʹ��ros::spin()�������ѭ����
    //ÿ���ж��ĵĻ��ⷢ��ʱ������ص��������պʹ�����Ϣ���ݡ�
    //���Ǹ����ʱ��һ���ڵ�����Ҫ���պʹ���ͬ��Դ�����ݣ�������Щ���ݵĲ���Ƶ��Ҳ������ͬ��
    //��������һ���ص�������ķ�̫��ʱ��ʱ���ᵼ�������ص��������������������ݶ�ʧ��
    //���ֳ�����Ҫ��һ���ڵ㿪�ٶ���̣߳���֤�������ĳ�ͨ��

    ros::MultiThreadedSpinner spinner(3);
    spinner.spin();
    
    return 0;
}
