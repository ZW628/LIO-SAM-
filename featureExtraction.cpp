#include "utility.h"
#include "lio_sam/cloud_info.h"

struct smoothness_t{ 
    float value;    // ����ֵ
    size_t ind;     // �����һά����
};

//���ʱȽϺ�������С��������
struct by_value{ 
    bool operator()(smoothness_t const &left, smoothness_t const &right) { 
        return left.value < right.value;
    }
};

class FeatureExtraction : public ParamServer
{

public:

    ros::Subscriber subLaserCloudInfo;

    // ������ǰ����֡��ȡ����֮��ĵ�����Ϣ
    ros::Publisher pubLaserCloudInfo;
    // ������ǰ����֡��ȡ�Ľǵ����
    ros::Publisher pubCornerPoints;
    // ������ǰ����֡��ȡ��ƽ������
    ros::Publisher pubSurfacePoints;

    // ��ǰ����֡�˶�����У�������Ч����
    pcl::PointCloud<PointType>::Ptr extractedCloud;
    // ��ǰ����֡�ǵ���Ƽ���
    pcl::PointCloud<PointType>::Ptr cornerCloud;
    // ��ǰ����֡ƽ�����Ƽ���
    pcl::PointCloud<PointType>::Ptr surfaceCloud;

    pcl::VoxelGrid<PointType> downSizeFilter;

    // ��ǰ����֡������Ϣ����������ʷ�����У�
    //�˶�����У�����������ݣ���ʼλ�ˣ���̬�ǣ���Ч�������ݣ��ǵ���ƣ�ƽ�����Ƶ�
    lio_sam::cloud_info cloudInfo;
    //���������ʼ�����м����
    std_msgs::Header cloudHeader;

    std::vector<smoothness_t> cloudSmoothness;
    float *cloudCurvature;
    // ������ȡ��ǣ�1��ʾ�ڵ���ƽ�У������Ѿ�����������ȡ�ĵ㣬0��ʾ��δ����������ȡ����
    int *cloudNeighborPicked;
    // 1��ʾ�ǵ㣬-1��ʾƽ���
    int *cloudLabel;

    FeatureExtraction()
    {
        // ���ĵ�ǰ����֡�˶�����У����ĵ�����Ϣ
        subLaserCloudInfo = nh.subscribe<lio_sam::cloud_info>("lio_sam/deskew/cloud_info", 1, &FeatureExtraction::laserCloudInfoHandler, this, ros::TransportHints().tcpNoDelay());

        // ������ǰ����֡��ȡ����֮��ĵ�����Ϣ
        pubLaserCloudInfo = nh.advertise<lio_sam::cloud_info> ("lio_sam/feature/cloud_info", 1);
        // ������ǰ����֡�Ľǵ����
        pubCornerPoints = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/feature/cloud_corner", 1);
        // ������ǰ����֡��������
        pubSurfacePoints = nh.advertise<sensor_msgs::PointCloud2>("lio_sam/feature/cloud_surface", 1);
        
        // ��ʼ��
        initializationValue();
    }

    void initializationValue()
    {
        cloudSmoothness.resize(N_SCAN*Horizon_SCAN);

        downSizeFilter.setLeafSize(odometrySurfLeafSize, odometrySurfLeafSize, odometrySurfLeafSize);

        extractedCloud.reset(new pcl::PointCloud<PointType>());
        cornerCloud.reset(new pcl::PointCloud<PointType>());
        surfaceCloud.reset(new pcl::PointCloud<PointType>());

        cloudCurvature = new float[N_SCAN*Horizon_SCAN];
        cloudNeighborPicked = new int[N_SCAN*Horizon_SCAN];
        cloudLabel = new int[N_SCAN*Horizon_SCAN];
    }

    //����imageProjection.cpp�з�����ȥ����ĵ��ƣ�ʵʱ����Ļص�����
    void laserCloudInfoHandler(const lio_sam::cloud_infoConstPtr& msgIn)
    {
        //msgIn��Ϊ�ص�������ȡ��ȥ���������Ϣ
        cloudInfo = *msgIn; // new cloud info
        cloudHeader = msgIn->header; // new cloud header
        pcl::fromROSMsg(msgIn->cloud_deskewed, *extractedCloud); // new cloud for extraction

        // ���㵱ǰ����֡������ÿ���������
        calculateSmoothness();

        // ��������ڵ���ƽ����������ĵ㣬����������ȡ
        markOccludedPoints();

        // ���ƽǵ㡢ƽ���������ȡ
        // 1������ɨ���ߣ�ÿ��ɨ����ɨ��һ�ܵĵ��ƻ���Ϊ6�Σ����ÿ����ȡ20���ǵ㡢����������ƽ��㣬����ǵ㼯�ϡ�ƽ��㼯��
        // 2����Ϊ�ǽǵ�ĵ㶼��ƽ��㣬����ƽ����Ƽ��ϣ���󽵲���
        extractFeatures();

        // �����ǵ㡢�����ƣ������������������ݵĵ�ǰ����֡������Ϣ
        publishFeatureCloud();
    }

    void calculateSmoothness()
    {
        // ������ǰ����֡�˶�����У�������Ч����
        int cloudSize = extractedCloud->points.size();
        for (int i = 5; i < cloudSize - 5; i++)
        {
            // �õ�ǰ�����ǰ��5������㵱ǰ�������
            //ע�⣬�����ǰ������㹲10���������������ȥ��10���ĵ�ǰ��
            float diffRange = cloudInfo.pointRange[i-5] + cloudInfo.pointRange[i-4]
                            + cloudInfo.pointRange[i-3] + cloudInfo.pointRange[i-2]
                            + cloudInfo.pointRange[i-1] - cloudInfo.pointRange[i] * 10
                            + cloudInfo.pointRange[i+1] + cloudInfo.pointRange[i+2]
                            + cloudInfo.pointRange[i+3] + cloudInfo.pointRange[i+4]
                            + cloudInfo.pointRange[i+5];            

            // �����ֵƽ����Ϊ����
            cloudCurvature[i] = diffRange*diffRange;//diffX * diffX + diffY * diffY + diffZ * diffZ;

            //0��ʾ��δ����������ȡ����,1��ʾ�ڵ���ƽ�У������Ѿ�����������ȡ�ĵ�
            cloudNeighborPicked[i] = 0;
            //1��ʾ�ǵ㣬-1��ʾƽ���
            cloudLabel[i] = 0;
            // cloudSmoothness for sorting
            // �洢�õ�����ֵ�������һά����
            //֮���Կ�����������������Ϊ��initializationValue���֣���cloudSmoothness���й���ʼ����
            //����ֱ�Ӷ�cloudSmoothness[i]��ֵ��һ���ᱨ�δ���
            cloudSmoothness[i].value = cloudCurvature[i];
            cloudSmoothness[i].ind = i;
        }
    }

    void markOccludedPoints()
    {
        int cloudSize = extractedCloud->points.size();
        // mark occluded points and parallel beam�������� points
        for (int i = 5; i < cloudSize - 6; ++i)
        {
            // occluded points
            // ��ǰ�����һ�����rangeֵ
            float depth1 = cloudInfo.pointRange[i];
            float depth2 = cloudInfo.pointRange[i+1];
           
            // ���������֮���һά������ֵ�������һ��ɨ�����ϣ���ôֵΪ1��
            //���������֮����һЩ��Ч�㱻�޳��ˣ����ܻ��1�󣬵������ر��
            // ���ǡ��ǰһ������ɨ��һ�ܵĽ���ʱ�̣���һ��������һ��ɨ���ߵ���ʼʱ�̣���ôֵ��ܴ�
            int columnDiff = std::abs(int(cloudInfo.pointColInd[i+1] - cloudInfo.pointColInd[i]));

            // ��������ͬһɨ�����ϣ��Ҿ���������0.3����Ϊ�����ڵ���ϵ
            //��Ҳ�����������㲻��ͬһƽ���ϣ������ͬһƽ���ϣ���������̫��
            //  Զ���ĵ�ᱻ�ڵ������һ�¸õ��Լ����ڵ�5���㣬���治�ٽ���������ȡ
            if (columnDiff < 10){
                // 10 pixel diff in range image
                if (depth1 - depth2 > 0.3){
                    cloudNeighborPicked[i - 5] = 1;
                    cloudNeighborPicked[i - 4] = 1;
                    cloudNeighborPicked[i - 3] = 1;
                    cloudNeighborPicked[i - 2] = 1;
                    cloudNeighborPicked[i - 1] = 1;
                    cloudNeighborPicked[i] = 1;
                }else if (depth2 - depth1 > 0.3){
                    cloudNeighborPicked[i + 1] = 1;
                    cloudNeighborPicked[i + 2] = 1;
                    cloudNeighborPicked[i + 3] = 1;
                    cloudNeighborPicked[i + 4] = 1;
                    cloudNeighborPicked[i + 5] = 1;
                    cloudNeighborPicked[i + 6] = 1;
                }
            }
            // parallel beam
            // ��ǰ�����ڵ��жϵ�ǰ������ƽ���Ƿ��뼤��������ƽ��
            //diff1��diff2�ǵ�ǰ�����ǰ��������ľ���
            float diff1 = std::abs(float(cloudInfo.pointRange[i-1] - cloudInfo.pointRange[i]));
            float diff2 = std::abs(float(cloudInfo.pointRange[i+1] - cloudInfo.pointRange[i]));

            //�����ǰ����������ڵ㶼��Զ��������Ϊ覵㣬��Ϊ����ǿ���̫С�������ϴ�
           // ѡ�����仯�ϴ�ĵ㣬�������Ǳ��Ϊ1
            if (diff1 > 0.02 * cloudInfo.pointRange[i] && diff2 > 0.02 * cloudInfo.pointRange[i])
                cloudNeighborPicked[i] = 1;
        }
    }

    void extractFeatures()
    {
        cornerCloud->clear();
        surfaceCloud->clear();

        pcl::PointCloud<PointType>::Ptr surfaceCloudScan(new pcl::PointCloud<PointType>());
        pcl::PointCloud<PointType>::Ptr surfaceCloudScanDS(new pcl::PointCloud<PointType>());

        for (int i = 0; i < N_SCAN; i++)
        {
            surfaceCloudScan->clear();
            // ��һ��ɨ����ɨ��һ�ܵĵ������ݣ�����Ϊ6�Σ�ÿ�ηֿ���ȡ������������������֤�������ȷֲ�
            for (int j = 0; j < 6; j++)
            {
                // ÿ�ε��Ƶ���ʼ������������startRingIndexΪɨ������ʼ��5���������һά�����е�����
                //ע�⣺���еĵ��������ﶼ����"һά����"����ʽ����
                //startRingIndex�� endRingIndex ��imageProjection.cpp�е� cloudExtraction�����ﱻ����
                //���� ��ǰring��һά��������ʼ����m����β��Ϊn��������n������ô6�ε���ʼ��ֱ�Ϊ��
                // m + [(n-m)/6]*j   j��0��5
                // ����Ϊ [��6-j)*m + nj ]/6
                // 6�ε���ֹ��ֱ�Ϊ��
                // m + (n-m)/6 + [(n-m)/6]*j -1  j��0��5,-1����Ϊ���һ��,��ȥ1
                // ����Ϊ [��5-j)*m + (j+1)*n ]/6 -1
                //��鲻��ϸ����Եֵ�����ǲ��ǻ��ֵ�׼�����翼��ǰ������ǲ��Ƕ���Ҫ������˵ֻ��Ҫǰ�ĸ��㣩��
                //ֻ�Ǿ����ܵķֿ������Σ���λ��ӵĵط���Ҫ����Ϊ�Ӵ�ĵ����У�һ��������ʵ�޹ؽ�Ҫ�� 
                int sp = (cloudInfo.startRingIndex[i] * (6 - j) + cloudInfo.endRingIndex[i] * j) / 6;
                int ep = (cloudInfo.startRingIndex[i] * (5 - j) + cloudInfo.endRingIndex[i] * (j + 1)) / 6 - 1;

                if (sp >= ep)
                    continue;

                // �������ʴ�С�����������
                //���Կ���֮ǰ��byvalue�����ﱻ�������жϺ�������
                std::sort(cloudSmoothness.begin()+sp, cloudSmoothness.begin()+ep, by_value());

                int largestPickedNum = 0;
                // �������ʴӴ�С����
                for (int k = ep; k >= sp; k--)
                {
                    // ����������
                    int ind = cloudSmoothness[k].ind;
                    // ��ǰ����㻹δ�����������ʴ�����ֵ������Ϊ�ǽǵ�
                    if (cloudNeighborPicked[ind] == 0 && cloudCurvature[ind] > edgeThreshold)
                    {
                        // ÿ��ֻȡ20���ǵ㣬�������ɨ����ɨ��һ����1800���㣬�򻮷�6�Σ�ÿ��300���㣬������ȡ20���ǵ�
                        largestPickedNum++;
                        if (largestPickedNum <= 20){
                            // ���Ϊ�ǵ�,����ǵ����
                            cloudLabel[ind] = 1;
                            cornerCloud->push_back(extractedCloud->points[ind]);
                        } else {
                            break;
                        }

                        // ����ѱ�����
                        cloudNeighborPicked[ind] = 1;
                        // ͬһ��ɨ�����Ϻ�5������һ�£����ٴ������������ۼ�
                        for (int l = 1; l <= 5; l++)
                        {
                            int columnDiff = std::abs(int(cloudInfo.pointColInd[ind + l] - cloudInfo.pointColInd[ind + l - 1]));
                            //����10��˵������Զ���������
                            if (columnDiff > 10)
                                break;
                            cloudNeighborPicked[ind + l] = 1;
                        }
                        // ͬһ��ɨ������ǰ5������һ�£����ٴ������������ۼ�
                        for (int l = -1; l >= -5; l--)
                        {
                            int columnDiff = std::abs(int(cloudInfo.pointColInd[ind + l] - cloudInfo.pointColInd[ind + l + 1]));
                            if (columnDiff > 10)
                                break;
                            cloudNeighborPicked[ind + l] = 1;
                        }
                    }
                }

                // �������ʴ�С�������
                for (int k = sp; k <= ep; k++)
                {
                    // ����������
                    int ind = cloudSmoothness[k].ind;
                    // ��ǰ����㻹δ������������С����ֵ������Ϊ��ƽ���
                    if (cloudNeighborPicked[ind] == 0 && cloudCurvature[ind] < surfThreshold)
                    {

                        // ���Ϊƽ���
                        cloudLabel[ind] = -1;
                        // ����ѱ�����
                        cloudNeighborPicked[ind] = 1;

                        // ͬһ��ɨ�����Ϻ�5������һ�£����ٴ������������ۼ�
                        for (int l = 1; l <= 5; l++) {

                            int columnDiff = std::abs(int(cloudInfo.pointColInd[ind + l] - cloudInfo.pointColInd[ind + l - 1]));
                            if (columnDiff > 10)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                        // ͬһ��ɨ������ǰ5������һ�£����ٴ������������ۼ�
                        for (int l = -1; l >= -5; l--) {

                            int columnDiff = std::abs(int(cloudInfo.pointColInd[ind + l] - cloudInfo.pointColInd[ind + l + 1]));
                            if (columnDiff > 10)
                                break;

                            cloudNeighborPicked[ind + l] = 1;
                        }
                    }
                }

                // ƽ����δ������ĵ�(<=0)������Ϊ��ƽ��㣬����ƽ����Ƽ���
                for (int k = sp; k <= ep; k++)
                {
                    if (cloudLabel[k] <= 0){
                        surfaceCloudScan->push_back(extractedCloud->points[k]);
                    }
                }
            }

            // ƽ����ƽ�����
            surfaceCloudScanDS->clear();
            downSizeFilter.setInputCloud(surfaceCloudScan);
            downSizeFilter.filter(*surfaceCloudScanDS);

            // ����ƽ����Ƽ���
            *surfaceCloud += *surfaceCloudScanDS;
            //��surfaceCloudScan��װ���ݣ�Ȼ��ŵ�downSizeFilter�
            //����downSizeFilter����.filter()�������ѽ�������*surfaceCloudScanDS�
            //����DSװ��surfaceCloud�С�DSָ����DownSample��
            //ͬ���ǵ㣨��Ե�㣩��û�������Ĳ�����ֱ�Ӿ���cornerCloud��װ���ơ�
        }
    }

    //����
    void freeCloudInfoMemory()
    {
        cloudInfo.startRingIndex.clear();
        cloudInfo.endRingIndex.clear();
        cloudInfo.pointColInd.clear();
        cloudInfo.pointRange.clear();
    }

    //�����ǵ㡢�����ƣ������������������ݵĵ�ǰ����֡������Ϣ
    void publishFeatureCloud()
    {
        // free cloud info memory
        freeCloudInfoMemory();
        // save newly extracted features
        // �����ǵ㡢�����ƣ�����rvizչʾ
        cloudInfo.cloud_corner  = publishCloud(pubCornerPoints,  cornerCloud,  cloudHeader.stamp, lidarFrame);
        cloudInfo.cloud_surface = publishCloud(pubSurfacePoints, surfaceCloud, cloudHeader.stamp, lidarFrame);
        // publish to mapOptimization
        // ������ǰ����֡������Ϣ�������˽ǵ㡢���������ݣ�������mapOptimization
        // ��imageProjection.cpp�����Ĳ���ͬһ�����⣬
        // image��������"lio_sam/deskew/cloud_info"��
        // ���﷢������"lio_sam/feature/cloud_info"��
        // ��˲��õ��ĵ�ͼ�Ż����ֵĳ�ͻ 
        pubLaserCloudInfo.publish(cloudInfo);
    }
};


int main(int argc, char** argv)
{
    ros::init(argc, argv, "lio_sam");

    FeatureExtraction FE;

    ROS_INFO("\033[1;32m----> Feature Extraction Started.\033[0m");
   
    ros::spin();

    return 0;
}